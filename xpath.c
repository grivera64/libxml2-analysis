/*
 * xpath.c: XML Path Language implementation
 *          XPath is a language for addressing parts of an XML document,
 *          designed to be used by both XSLT and XPointer
 *
 * Reference: W3C Recommendation 16 November 1999
 *     http://www.w3.org/TR/1999/REC-xpath-19991116
 * Public reference:
 *     http://www.w3.org/TR/xpath
 *
 * See Copyright for the status of this software
 *
 * Author: daniel@veillard.com
 *
 */

/* To avoid EBCDIC trouble when parsing on zOS */
#if defined(__MVS__)
#pragma convert("ISO8859-1")
#endif

#define IN_LIBXML
#include "libxml.h"

#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <ctype.h>

#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/parserInternals.h>
#include <libxml/hash.h>
#ifdef LIBXML_DEBUG_ENABLED
#include <libxml/debugXML.h>
#endif
#include <libxml/xmlerror.h>
#include <libxml/threads.h>
#ifdef LIBXML_PATTERN_ENABLED
#include <libxml/pattern.h>
#endif

#include "private/buf.h"
#include "private/error.h"
#include "private/memory.h"
#include "private/parser.h"
#include "private/xpath.h"

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

/* Disabled for now */
#if 0
#ifdef LIBXML_PATTERN_ENABLED
#define XPATH_STREAMING
#endif
#endif

/**
 * WITH_TIM_SORT:
 *
 * Use the Timsort algorithm provided in timsort.h to sort
 * nodeset as this is a great improvement over the old Shell sort
 * used in xmlXPathNodeSetSort()
 */
#define WITH_TIM_SORT

/*
* XP_OPTIMIZED_NON_ELEM_COMPARISON:
* If defined, this will use xmlXPathCmpNodesExt() instead of
* xmlXPathCmpNodes(). The new function is optimized comparison of
* non-element nodes; actually it will speed up comparison only if
* xmlXPathOrderDocElems() was called in order to index the elements of
* a tree in document order; Libxslt does such an indexing, thus it will
* benefit from this optimization.
*/
#define XP_OPTIMIZED_NON_ELEM_COMPARISON

/*
* XP_OPTIMIZED_FILTER_FIRST:
* If defined, this will optimize expressions like "key('foo', 'val')[b][1]"
* in a way, that it stop evaluation at the first node.
*/
#define XP_OPTIMIZED_FILTER_FIRST

/*
 * XPATH_MAX_STEPS:
 * when compiling an XPath expression we arbitrary limit the maximum
 * number of step operation in the compiled expression. 1000000 is
 * an insanely large value which should never be reached under normal
 * circumstances
 */
#define XPATH_MAX_STEPS 1000000

/*
 * XPATH_MAX_STACK_DEPTH:
 * when evaluating an XPath expression we arbitrary limit the maximum
 * number of object allowed to be pushed on the stack. 1000000 is
 * an insanely large value which should never be reached under normal
 * circumstances
 */
#define XPATH_MAX_STACK_DEPTH 1000000

/*
 * XPATH_MAX_NODESET_LENGTH:
 * when evaluating an XPath expression nodesets are created and we
 * arbitrary limit the maximum length of those node set.
 */
#define XPATH_MAX_NODESET_LENGTH 100000000

/*
 * XPATH_MAX_RECURSION_DEPTH:
 * Maximum amount of nested functions calls when parsing or evaluating
 * expressions. Each increase should represent roughly 100 bytes of
 * stack space. Sanitizers have much higher stack usage.
 */
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#define XPATH_MAX_RECURSION_DEPTH 500
#elif defined(_WIN32)
/* Windows typically limits stack size to 1MB. */
#define XPATH_MAX_RECURSION_DEPTH 1000
#else
#define XPATH_MAX_RECURSION_DEPTH 5000
#endif

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
  #define XML_NODESET_DEFAULT  1
#else
  #define XML_NODESET_DEFAULT 10
#endif

#define AXIS_IS_REVERSE(axis) ((1 << (axis)) & ( \
    (1 << AXIS_ANCESTOR) | \
    (1 << AXIS_ANCESTOR_OR_SELF) | \
    (1 << AXIS_PRECEDING) | \
    (1 << AXIS_PRECEDING_SIBLING)))

#define TYPE_MASK_DOC       ((1 << XML_DOCUMENT_NODE) | \
                             (1 << XML_HTML_DOCUMENT_NODE))
#define TYPE_MASK_ELEM       (1 << XML_ELEMENT_NODE)
#define TYPE_MASK_TEXT      ((1 << XML_TEXT_NODE) | \
                             (1 << XML_CDATA_SECTION_NODE))
#define TYPE_MASK_COMMENT    (1 << XML_COMMENT_NODE)
#define TYPE_MASK_PI         (1 << XML_PI_NODE)
#define TYPE_MASK_ATTR       (1 << XML_ATTRIBUTE_NODE)
#define TYPE_MASK_NS         (1 << XML_NAMESPACE_DECL)

#define TYPE_MASK_NODE      (TYPE_MASK_DOC | \
                             TYPE_MASK_ELEM | \
                             TYPE_MASK_TEXT | \
                             TYPE_MASK_COMMENT | \
                             TYPE_MASK_PI | \
                             TYPE_MASK_ATTR | \
                             TYPE_MASK_NS)

/*
 * TODO:
 * There are a few spots where some tests are done which depend upon ascii
 * data.  These should be enhanced for full UTF8 support (see particularly
 * any use of the macros IS_ASCII_CHARACTER and IS_ASCII_DIGIT)
 */

#if defined(LIBXML_XPATH_ENABLED)

typedef enum {
    XPATH_OP_END=0,

    /* conversion */
    XPATH_OP_BOOL,          /*  2,100,000 - 1*/
    XPATH_OP_NOT,           /*    900,000 */
    XPATH_OP_NUMBER,        /*    700,000 */
    XPATH_OP_STRING,        /*  1,500,000 */
    XPATH_OP_NODESET,       /*  3,900,000 */
    XPATH_OP_XSLT_TREE,     /*    100,000 */

    /* binary bool ops */
    XPATH_OP_AND,           /*  1,600,000 - 7 */
    XPATH_OP_OR,            /*  1,900,000 */

    /* relational ops */
    XPATH_OP_EQ,            /*  2,200,000 - 9 */
    XPATH_OP_NE,            /*  1,700,000 */
    XPATH_OP_LT,            /*  1,000,000 */
    XPATH_OP_LE,

    XPATH_OP_EQ_NUM,        /*    100,000 - 13 */
    XPATH_OP_NE_NUM,        /*    300,000 */
    XPATH_OP_LT_NUM,        /*  4,500,000 */
    XPATH_OP_LE_NUM,

    XPATH_OP_EQ_STR,        /*  3,200,000 - 17 */
    XPATH_OP_NE_STR,

    /* unary math ops */
    XPATH_OP_NEG,
    XPATH_OP_FLOOR,
    XPATH_OP_CEIL,
    XPATH_OP_ROUND,

    /* binary math ops */
    XPATH_OP_ADD,           /*    600,000 - 23 */
    XPATH_OP_SUB,
    XPATH_OP_MULT,
    XPATH_OP_DIV,
    XPATH_OP_MOD,

    XPATH_OP_UNION,         /*  1,700,000 - 28 */
    XPATH_OP_ROOT,
    XPATH_OP_NODE,          /*  1,000,000 */
    XPATH_OP_STEP,          /*  3,500,000 */
    XPATH_OP_STEP_CTXT,     /*  6,200,000 */
    XPATH_OP_VALUE_BOOL,
    XPATH_OP_VALUE_NUMBER,  /*  6,900,000 */
    XPATH_OP_VALUE_STRING,  /*  5,900,000 */
    XPATH_OP_VARIABLE,      /* 17,200,000 */
    XPATH_OP_SFUNC,         /*  1,200,000 */
    XPATH_OP_FUNCTION,      /*  1,300,000 */
    XPATH_OP_ARG,           /*  1,200,000 */
    XPATH_OP_PREDICATE,
    XPATH_OP_FILTER,        /*    600,000 */
    XPATH_OP_SORT,          /*  4,100,000 */

    /* nullary ops */
    XPATH_OP_POSITION,      /*  5,000,000 - 43 */
    XPATH_OP_LAST,

    /* name ops */
    XPATH_OP_LOCAL_NAME,
    XPATH_OP_LOCAL_NAME_CTXT,   /* 4,200,000 - 47 */
    XPATH_OP_NAME,
    XPATH_OP_NAME_CTXT,
    XPATH_OP_NAMESPACE_URI,
    XPATH_OP_NAMESPACE_URI_CTXT,

    /* Compiled to XPATH_OP_VAR */
    XPATH_OP_TRUE,
    XPATH_OP_FALSE
} xmlXPathOpcode;

typedef enum {
    XPATH_EVAL_ALL = 0,
    XPATH_EVAL_FIRST = 1,
    /* values 2-249 mean the specific index */
    XPATH_EVAL_LAST = 250,
    XPATH_EVAL_ANY = 251,
    XPATH_EVAL_NONE = 252,
    XPATH_EVAL_DEFAULT = 253
} xmlXPathEvalMode;

typedef struct _xmlXPathStandardFunction xmlXPathStandardFunction;

typedef int
(*xmlXPathFuncCompiler)(xmlXPathContextPtr ctxt,
                        const xmlXPathStandardFunction *sfunc, int nargs,
                        int argIndex);

typedef struct {
    unsigned char type;
    unsigned char isCopy;
    unsigned char hasNsNodes;

    union {
        int boolean;
        double number;
        xmlChar *string;
        xmlNodeSet nodeset;
        void *user;
    } as;
} xmlXPathItem;

struct _xmlXPathStandardFunction {
    const char *name;
    xmlXPathFunction func;
    xmlXPathFuncCompiler compiler;
    xmlXPathOpcode op;
    xmlXPathObjectType retType;
    int minArgs;
    int maxArgs;
    xmlXPathObjectType arg1Type;
    xmlXPathObjectType arg2Type;
};

static int
xmlXPathTrueCompiler(xmlXPathContextPtr ctxt,
                     const xmlXPathStandardFunction *sfunc, int nargs,
                     int argIndex);

static int
xmlXPathNameCompiler(xmlXPathContextPtr ctxt,
                     const xmlXPathStandardFunction *sfunc, int nargs,
                     int argIndex);

static int
xmlXPathNotCompiler(xmlXPathContextPtr ctxt,
                    const xmlXPathStandardFunction *sfunc, int nargs,
                    int argIndex);

/*
 * Note that we allow XSLT result tree fragments (XPATH_XSLT_TREE)
 * as nodeset function arguments. This seems to violate the XSLT 1.0
 * spec which says:
 *
 * > An operation is permitted on a result tree fragment only if that
 * > operation would be permitted on a string.
 *
 * For example count($var) should be illegal. But allowing RTFs has
 * been long-standing behavior.
 */
static const xmlXPathStandardFunction xmlXPathStandardFunctions[] = {
    { "boolean", NULL, NULL,
        XPATH_OP_BOOL, XPATH_BOOLEAN, 1, 1, XPATH_BOOLEAN, 0 },
    { "ceiling", NULL, NULL,
        XPATH_OP_CEIL, XPATH_NUMBER, 1, 1, XPATH_NUMBER, 0 },
    { "count", xmlXPathCountFunction, NULL,
        XPATH_OP_SFUNC, XPATH_NUMBER, 1, 1, XPATH_XSLT_TREE, 0 },
    { "concat", xmlXPathConcatFunction, NULL,
        XPATH_OP_SFUNC, XPATH_STRING,
        2, INT_MAX, XPATH_STRING, XPATH_STRING },
    { "contains", xmlXPathContainsFunction, NULL,
        XPATH_OP_SFUNC, XPATH_BOOLEAN,
        2, 2, XPATH_STRING, XPATH_STRING },
    { "id", xmlXPathIdFunction, NULL,
        XPATH_OP_SFUNC, XPATH_NODESET, 1, 1, XPATH_UNDEFINED, 0 },
    { "false", NULL, xmlXPathTrueCompiler,
        XPATH_OP_FALSE, XPATH_BOOLEAN, 0, 0, 0, 0 },
    { "floor", NULL, NULL,
        XPATH_OP_FLOOR, XPATH_NUMBER, 1, 1, XPATH_NUMBER, 0 },
    { "last", NULL, NULL,
        XPATH_OP_LAST, XPATH_NUMBER, 0, 0, 0, 0 },
    { "lang", xmlXPathLangFunction, NULL,
        XPATH_OP_SFUNC, XPATH_BOOLEAN, 1, 1, XPATH_STRING, 0 },
    { "local-name", NULL, xmlXPathNameCompiler,
        XPATH_OP_LOCAL_NAME, XPATH_STRING, 0, 1, XPATH_XSLT_TREE, 0 },
    { "not", NULL, xmlXPathNotCompiler,
        XPATH_OP_NOT, XPATH_BOOLEAN, 1, 1, XPATH_UNDEFINED, 0 },
    { "name", NULL, xmlXPathNameCompiler,
        XPATH_OP_NAME, XPATH_STRING, 0, 1, XPATH_XSLT_TREE, 0 },
    { "namespace-uri", NULL, xmlXPathNameCompiler,
        XPATH_OP_NAMESPACE_URI, XPATH_STRING, 0, 1, XPATH_XSLT_TREE, 0 },
    { "normalize-space", xmlXPathNormalizeFunction, NULL,
        XPATH_OP_SFUNC, XPATH_STRING, 0, 1, XPATH_STRING, 0 },
    { "number", xmlXPathNumberFunction, NULL,
        XPATH_OP_NUMBER, XPATH_NUMBER, 0, 1, XPATH_NUMBER, 0 },
    { "position", NULL, NULL,
        XPATH_OP_POSITION, XPATH_NUMBER, 0, 0, 0, 0 },
    { "round", NULL, NULL,
        XPATH_OP_ROUND, XPATH_NUMBER, 1, 1, XPATH_NUMBER, 0 },
    { "string", xmlXPathStringFunction, NULL,
        XPATH_OP_STRING, XPATH_STRING, 0, 1, XPATH_STRING, 0 },
    { "string-length", xmlXPathStringLengthFunction, NULL,
        XPATH_OP_SFUNC, XPATH_NUMBER, 0, 1, XPATH_STRING, 0 },
    { "starts-with", xmlXPathStartsWithFunction, NULL,
        XPATH_OP_SFUNC, XPATH_BOOLEAN,
        2, 2, XPATH_STRING, XPATH_STRING },
    { "substring", xmlXPathSubstringFunction, NULL,
        XPATH_OP_SFUNC, XPATH_STRING,
        2, 3, XPATH_STRING, XPATH_NUMBER },
    { "substring-after", xmlXPathSubstringAfterFunction, NULL,
        XPATH_OP_SFUNC, XPATH_STRING,
        2, 2, XPATH_STRING, XPATH_STRING },
    { "substring-before", xmlXPathSubstringBeforeFunction, NULL,
        XPATH_OP_SFUNC, XPATH_STRING,
        2, 2, XPATH_STRING, XPATH_STRING },
    { "sum", xmlXPathSumFunction, NULL,
        XPATH_OP_SFUNC, XPATH_NUMBER, 1, 1, XPATH_XSLT_TREE, 0 },
    { "true", NULL, xmlXPathTrueCompiler,
        XPATH_OP_TRUE, XPATH_BOOLEAN, 0, 0, 0, 0 },
    { "translate", xmlXPathTranslateFunction, NULL,
        XPATH_OP_SFUNC, XPATH_STRING,
        3, 3, XPATH_STRING, XPATH_STRING }
};

#define NUM_STANDARD_FUNCTIONS \
    (sizeof(xmlXPathStandardFunctions) / sizeof(xmlXPathStandardFunctions[0]))

#define SF_HASH_SIZE 64

static unsigned char xmlXPathSFHash[SF_HASH_SIZE];

double xmlXPathNAN = 0.0;
double xmlXPathPINF = 0.0;
double xmlXPathNINF = 0.0;

/**
 * xmlXPathInit:
 *
 * DEPRECATED: Alias for xmlInitParser.
 */
void
xmlXPathInit(void) {
    xmlInitParser();
}

ATTRIBUTE_NO_SANITIZE_INTEGER
static unsigned
xmlXPathSFComputeHash(const xmlChar *name) {
    unsigned hashValue = 5381;
    const xmlChar *ptr;

    for (ptr = name; *ptr; ptr++)
        hashValue = hashValue * 33 + *ptr;

    return(hashValue);
}

/**
 * xmlInitXPathInternal:
 *
 * Initialize the XPath environment
 */
ATTRIBUTE_NO_SANITIZE("float-divide-by-zero")
void
xmlInitXPathInternal(void) {
    size_t i;

#if defined(NAN) && defined(INFINITY)
    xmlXPathNAN = NAN;
    xmlXPathPINF = INFINITY;
    xmlXPathNINF = -INFINITY;
#else
    /* MSVC doesn't allow division by zero in constant expressions. */
    double zero = 0.0;
    xmlXPathNAN = 0.0 / zero;
    xmlXPathPINF = 1.0 / zero;
    xmlXPathNINF = -xmlXPathPINF;
#endif

    /*
     * Initialize hash table for standard functions
     */

    for (i = 0; i < SF_HASH_SIZE; i++)
        xmlXPathSFHash[i] = UCHAR_MAX;

    for (i = 0; i < NUM_STANDARD_FUNCTIONS; i++) {
        const char *name = xmlXPathStandardFunctions[i].name;
        int bucketIndex = xmlXPathSFComputeHash(BAD_CAST name) % SF_HASH_SIZE;

        while (xmlXPathSFHash[bucketIndex] != UCHAR_MAX) {
            bucketIndex += 1;
            if (bucketIndex >= SF_HASH_SIZE)
                bucketIndex = 0;
        }

        xmlXPathSFHash[bucketIndex] = i;
    }
}

/************************************************************************
 *									*
 *			Floating point stuff				*
 *									*
 ************************************************************************/

/**
 * xmlXPathIsNaN:
 * @val:  a double value
 *
 * Checks whether a double is a NaN.
 *
 * Returns 1 if the value is a NaN, 0 otherwise
 */
int
xmlXPathIsNaN(double val) {
#ifdef isnan
    return isnan(val);
#else
    return !(val == val);
#endif
}

/**
 * xmlXPathIsInf:
 * @val:  a double value
 *
 * Checks whether a double is an infinity.
 *
 * Returns 1 if the value is +Infinite, -1 if -Infinite, 0 otherwise
 */
int
xmlXPathIsInf(double val) {
#ifdef isinf
    return isinf(val) ? (val > 0 ? 1 : -1) : 0;
#else
    if (val >= xmlXPathPINF)
        return 1;
    if (val <= -xmlXPathPINF)
        return -1;
    return 0;
#endif
}

static double
xmlXPathRound(double f) {
    if ((f >= -0.5) && (f < 0.5)) {
        /* Handles negative zero. */
        return(f * 0.0);
    }
    else {
        double rounded = floor(f);

        if (f - rounded >= 0.5)
            rounded += 1.0;

        return(rounded);
    }
}

/*
 * TODO: when compatibility allows remove all "fake node libxslt" strings
 *       the test should just be name[0] = ' '
 */

static const xmlNs xmlXPathXMLNamespaceStruct = {
    NULL,
    XML_NAMESPACE_DECL,
    XML_XML_NAMESPACE,
    BAD_CAST "xml",
    NULL,
    NULL
};
static const xmlNs *const xmlXPathXMLNamespace = &xmlXPathXMLNamespaceStruct;

#define XML_NODE_SORT_VALUE(n) XML_PTR_TO_INT((n)->content)

#ifdef XP_OPTIMIZED_NON_ELEM_COMPARISON

/**
 * xmlXPathCmpNodesExt:
 * @node1:  the first node
 * @node2:  the second node
 *
 * Compare two nodes w.r.t document order.
 * This one is optimized for handling of non-element nodes.
 *
 * Returns -2 in case of error 1 if first point < second point, 0 if
 *         it's the same node, -1 otherwise
 */
static int
xmlXPathCmpNodesExt(xmlNodePtr node1, xmlNodePtr node2) {
    int depth1, depth2;
    int misc = 0, precedence1 = 0, precedence2 = 0;
    xmlNodePtr miscNode1 = NULL, miscNode2 = NULL;
    xmlNodePtr cur, root;
    XML_INTPTR_T l1, l2;

    if ((node1 == NULL) || (node2 == NULL))
	return(-2);

    if (node1 == node2)
	return(0);

    /*
     * a couple of optimizations which will avoid computations in most cases
     */
    switch (node1->type) {
	case XML_ELEMENT_NODE:
	    if (node2->type == XML_ELEMENT_NODE) {
		if ((0 > XML_NODE_SORT_VALUE(node1)) &&
		    (0 > XML_NODE_SORT_VALUE(node2)) &&
		    (node1->doc == node2->doc))
		{
		    l1 = -XML_NODE_SORT_VALUE(node1);
		    l2 = -XML_NODE_SORT_VALUE(node2);
		    if (l1 < l2)
			return(1);
		    if (l1 > l2)
			return(-1);
		} else
		    goto turtle_comparison;
	    }
	    break;
	case XML_NAMESPACE_DECL: {
            xmlNsPtr ns = (xmlNsPtr) node1;

	    precedence1 = 1; /* element is owner */
	    miscNode1 = node1;
	    node1 = (xmlNodePtr) ns->next;
	    misc = 1;
	    break;
        }
	case XML_ATTRIBUTE_NODE:
	    precedence1 = 2; /* element is owner */
	    miscNode1 = node1;
	    node1 = node1->parent;
	    misc = 1;
	    break;
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_COMMENT_NODE:
	case XML_PI_NODE: {
	    miscNode1 = node1;
	    /*
	    * Find nearest element node.
	    */
	    if (node1->prev != NULL) {
		do {
		    node1 = node1->prev;
		    if (node1->type == XML_ELEMENT_NODE) {
			precedence1 = 4; /* element in prev-sibl axis */
			break;
		    }
		    if (node1->prev == NULL) {
			precedence1 = 3; /* element is parent */
			/*
			* URGENT TODO: Are there any cases, where the
			* parent of such a node is not an element node?
			*/
			node1 = node1->parent;
			break;
		    }
		} while (1);
	    } else {
		precedence1 = 3; /* element is parent */
		node1 = node1->parent;
	    }
	    if ((node1 == NULL) || (node1->type != XML_ELEMENT_NODE) ||
		(0 <= XML_NODE_SORT_VALUE(node1))) {
		/*
		* Fallback for whatever case.
		*/
		node1 = miscNode1;
		precedence1 = 0;
	    } else
		misc = 1;
	}
	    break;
	default:
	    break;
    }
    switch (node2->type) {
	case XML_ELEMENT_NODE:
	    break;
	case XML_NAMESPACE_DECL: {
            xmlNsPtr ns = (xmlNsPtr) node2;

	    precedence2 = 1; /* element is owner */
	    miscNode2 = node2;
	    node2 = (xmlNodePtr) ns->next;
	    misc = 1;
	    break;
        }
	case XML_ATTRIBUTE_NODE:
	    precedence2 = 2; /* element is owner */
	    miscNode2 = node2;
	    node2 = node2->parent;
	    misc = 1;
	    break;
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_COMMENT_NODE:
	case XML_PI_NODE: {
	    miscNode2 = node2;
	    if (node2->prev != NULL) {
		do {
		    node2 = node2->prev;
		    if (node2->type == XML_ELEMENT_NODE) {
			precedence2 = 4; /* element in prev-sibl axis */
			break;
		    }
		    if (node2->prev == NULL) {
			precedence2 = 3; /* element is parent */
			node2 = node2->parent;
			break;
		    }
		} while (1);
	    } else {
		precedence2 = 3; /* element is parent */
		node2 = node2->parent;
	    }
	    if ((node2 == NULL) || (node2->type != XML_ELEMENT_NODE) ||
		(0 <= XML_NODE_SORT_VALUE(node2)))
	    {
		node2 = miscNode2;
		precedence2 = 0;
	    } else
		misc = 1;
	}
	    break;
	default:
	    break;
    }
    if (misc) {
	if (node1 == node2) {
	    if (precedence1 == precedence2) {
                if (precedence1 == 1) {
                    xmlNsPtr ns1 = (xmlNsPtr) miscNode1;
                    xmlNsPtr ns2 = (xmlNsPtr) miscNode2;

                    return(xmlStrcmp(ns2->prefix, ns1->prefix));
                }

		/*
		* The ugly case; but normally there aren't many
		* adjacent non-element nodes around.
		*/
		cur = miscNode2->prev;
		while (cur != NULL) {
		    if (cur == miscNode1)
			return(1);
		    if (cur->type == XML_ELEMENT_NODE)
			return(-1);
		    cur = cur->prev;
		}
		return (-1);
	    } else {
		/*
		* Evaluate based on higher precedence wrt to the element.
		*/
		if (precedence1 < precedence2)
		    return(1);
		else
		    return(-1);
	    }
	}
	/*
	* Special case: One of the helper-elements is contained by the other.
	* <foo>
	*   <node2>
	*     <node1>Text-1(precedence1 == 3)</node1>
	*   </node2>
	*   Text-6(precedence2 == 4)
	* </foo>
	*/
	if ((precedence2 == 4) && (precedence1 > 2)) {
	    cur = node1->parent;
	    while (cur) {
		if (cur == node2)
		    return(1);
		cur = cur->parent;
	    }
	}
	if ((precedence1 == 4) && (precedence2 > 2)) {
	    cur = node2->parent;
	    while (cur) {
		if (cur == node1)
		    return(-1);
		cur = cur->parent;
	    }
	}
    }

    /*
     * Speedup using document order if available.
     */
    if ((node1->type == XML_ELEMENT_NODE) &&
	(node2->type == XML_ELEMENT_NODE) &&
	(0 > XML_NODE_SORT_VALUE(node1)) &&
	(0 > XML_NODE_SORT_VALUE(node2)) &&
	(node1->doc == node2->doc)) {

	l1 = -XML_NODE_SORT_VALUE(node1);
	l2 = -XML_NODE_SORT_VALUE(node2);
	if (l1 < l2)
	    return(1);
	if (l1 > l2)
	    return(-1);
    }

turtle_comparison:

    if (node1 == node2->prev)
	return(1);
    if (node1 == node2->next)
	return(-1);
    /*
     * compute depth to root
     */
    for (depth2 = 0, cur = node2; cur->parent != NULL; cur = cur->parent) {
	if (cur->parent == node1)
	    return(1);
	depth2++;
    }
    root = cur;
    for (depth1 = 0, cur = node1; cur->parent != NULL; cur = cur->parent) {
	if (cur->parent == node2)
	    return(-1);
	depth1++;
    }
    /*
     * Distinct document (or distinct entities :-( ) case.
     */
    if (root != cur) {
	return(-2);
    }
    /*
     * get the nearest common ancestor.
     */
    while (depth1 > depth2) {
	depth1--;
	node1 = node1->parent;
    }
    while (depth2 > depth1) {
	depth2--;
	node2 = node2->parent;
    }
    while (node1->parent != node2->parent) {
	node1 = node1->parent;
	node2 = node2->parent;
	/* should not happen but just in case ... */
	if ((node1 == NULL) || (node2 == NULL))
	    return(-2);
    }
    /*
     * Find who's first.
     */
    if (node1 == node2->prev)
	return(1);
    if (node1 == node2->next)
	return(-1);
    /*
     * Speedup using document order if available.
     */
    if ((node1->type == XML_ELEMENT_NODE) &&
	(node2->type == XML_ELEMENT_NODE) &&
	(0 > XML_NODE_SORT_VALUE(node1)) &&
	(0 > XML_NODE_SORT_VALUE(node2)) &&
	(node1->doc == node2->doc)) {

	l1 = -XML_NODE_SORT_VALUE(node1);
	l2 = -XML_NODE_SORT_VALUE(node2);
	if (l1 < l2)
	    return(1);
	if (l1 > l2)
	    return(-1);
    }

    for (cur = node1->next;cur != NULL;cur = cur->next)
	if (cur == node2)
	    return(1);
    return(-1); /* assume there is no sibling list corruption */
}
#endif /* XP_OPTIMIZED_NON_ELEM_COMPARISON */

/*
 * Wrapper for the Timsort algorithm from timsort.h
 */
#ifdef WITH_TIM_SORT
#define SORT_NAME libxml_domnode
#define SORT_TYPE xmlNodePtr
/**
 * wrap_cmp:
 * @x: a node
 * @y: another node
 *
 * Comparison function for the Timsort implementation
 *
 * Returns -2 in case of error -1 if first point < second point, 0 if
 *         it's the same node, +1 otherwise
 */
static
int wrap_cmp( xmlNodePtr x, xmlNodePtr y );
#ifdef XP_OPTIMIZED_NON_ELEM_COMPARISON
    static int wrap_cmp( xmlNodePtr x, xmlNodePtr y )
    {
        int res = xmlXPathCmpNodesExt(x, y);
        return res == -2 ? res : -res;
    }
#else
    static int wrap_cmp( xmlNodePtr x, xmlNodePtr y )
    {
        int res = xmlXPathCmpNodes(x, y);
        return res == -2 ? res : -res;
    }
#endif
#define SORT_CMP(x, y)  (wrap_cmp(x, y))
#include "timsort.h"
#endif /* WITH_TIM_SORT */

/************************************************************************
 *									*
 *			Error handling routines				*
 *									*
 ************************************************************************/

/*
 * The array xmlXPathErrorMessages corresponds to the enum xmlXPathError
 */
static const char* const xmlXPathErrorMessages[] = {
    "Ok\n",
    "Number encoding\n",
    "Unfinished literal\n",
    "Start of literal\n",
    "Expected $ for variable reference\n",
    "Undefined variable\n",
    "Invalid predicate\n",
    "Invalid expression\n",
    "Missing closing curly brace\n",
    "Unregistered function\n",
    "Invalid operand\n",
    "Invalid type\n",
    "Invalid number of arguments\n",
    "Invalid context size\n",
    "Invalid context position\n",
    "Memory allocation error\n",
    "Syntax error\n",
    "Resource error\n",
    "Sub resource error\n",
    "Undefined namespace prefix\n",
    "Encoding error\n",
    "Char out of XML range\n",
    "Invalid or incomplete context\n",
    "Stack usage error\n",
    "Forbidden variable\n",
    "Operation limit exceeded\n",
    "Recursion limit exceeded\n",
    "?? Unknown error ??\n"	/* Must be last in the list! */
};
#define MAXERRNO ((int)(sizeof(xmlXPathErrorMessages) /	\
		   sizeof(xmlXPathErrorMessages[0])) - 1)
/**
 * xmlXPathErrMemory:
 * @ctxt:  an XPath context
 *
 * Handle a memory allocation failure.
 */
ATTRIBUTE_NO_INLINE
void
xmlXPathErrMemory(xmlXPathContextPtr ctxt) {
    if (ctxt == NULL)
        return;
    ctxt->pctxt.error = XPATH_MEMORY_ERROR;
    xmlRaiseMemoryError(ctxt->serror, NULL, ctxt->userData, XML_FROM_XPATH,
                        &ctxt->lastError);
}

ATTRIBUTE_NO_INLINE
static void
xmlXPathCErr(xmlXPathContextPtr ctxt, int code) {
    xmlErrorPtr err;
    xmlStructuredErrorFunc schannel = NULL;
    xmlGenericErrorFunc channel = NULL;
    void *data = NULL;
    xmlNodePtr node = NULL;
    int res;

    if (ctxt == NULL)
        return;
    if ((code < 0) || (code > MAXERRNO))
	code = MAXERRNO;
    /* Only report the first error */
    if (ctxt->pctxt.error != 0)
        return;

    ctxt->pctxt.error = code;

    err = &ctxt->lastError;

    /* Don't overwrite memory error. */
    if (err->code == XML_ERR_NO_MEMORY)
        return;

    /* cleanup current last error */
    xmlResetError(err);

    err->domain = XML_FROM_XPATH;
    err->code = code + XML_XPATH_EXPRESSION_OK - XPATH_EXPRESSION_OK;
    err->level = XML_ERR_ERROR;
    err->message = xmlMemStrdup(xmlXPathErrorMessages[code]);
    if (err->message == NULL) {
        xmlXPathErrMemory(ctxt);
        return;
    }
    if (ctxt->pctxt.base != NULL) {
        err->str1 = (char *) xmlStrdup(ctxt->pctxt.base);
        if (err->str1 == NULL) {
            xmlXPathErrMemory(ctxt);
            return;
        }
    }
    err->int1 = ctxt->pctxt.cur - ctxt->pctxt.base;
    err->node = ctxt->debugNode;

    schannel = ctxt->serror;
    data = ctxt->userData;
    node = ctxt->debugNode;

    if (schannel == NULL) {
        channel = xmlGenericError;
        data = xmlGenericErrorContext;
    }

    res = xmlRaiseError(schannel, channel, data, NULL, node, XML_FROM_XPATH,
                        code + XML_XPATH_EXPRESSION_OK - XPATH_EXPRESSION_OK,
                        XML_ERR_ERROR, NULL, 0,
                        (const char *) ctxt->pctxt.base, NULL, NULL,
                        ctxt->pctxt.cur - ctxt->pctxt.base, 0,
                        "%s", xmlXPathErrorMessages[code]);
    if (res < 0)
        xmlXPathErrMemory(ctxt);
}

/**
 * xmlXPathErr:
 * @ctxt:  a XPath parser context
 * @code:  the error code
 *
 * Handle an XPath error
 */
void
xmlXPathErr(xmlXPathParserContextPtr ctxt, int code)
{
    if (ctxt == NULL)
        return;

    xmlXPathCErr(ctxt->context, code);
}

/**
 * xmlXPatherror:
 * @ctxt:  the XPath Parser context
 * @file:  the file name
 * @line:  the line number
 * @no:  the error number
 *
 * DEPRECATED: Use xmlXPathErr.
 *
 * Formats an error message.
 */
void
xmlXPatherror(xmlXPathParserContextPtr ctxt, const char *file ATTRIBUTE_UNUSED,
              int line ATTRIBUTE_UNUSED, int no) {
    if (ctxt == NULL)
        return;

    xmlXPathCErr(ctxt->context, no);
}

/**
 * xmlXPathCheckOpLimit:
 * @ctxt:  the XPath Parser context
 * @opCount:  the number of operations to be added
 *
 * Adds opCount to the running total of operations and returns -1 if the
 * operation limit is exceeded. Returns 0 otherwise.
 */
static int
xmlXPathCheckOpLimit(xmlXPathContextPtr ctxt, unsigned long opCount) {
    if ((opCount > ctxt->opLimit) ||
        (ctxt->opCount > ctxt->opLimit - opCount)) {
        ctxt->opCount = ctxt->opLimit;
        xmlXPathCErr(ctxt, XPATH_OP_LIMIT_EXCEEDED);
        return(-1);
    }

    ctxt->opCount += opCount;
    return(0);
}

#define OP_LIMIT_EXCEEDED(ctxt, n) \
    ((ctxt->opLimit != 0) && (xmlXPathCheckOpLimit(ctxt, n) < 0))

/************************************************************************
 *									*
 *			Parser Types					*
 *									*
 ************************************************************************/

/*
 * Types are private:
 */

typedef enum {
    AXIS_ANCESTOR = 1,
    AXIS_ANCESTOR_OR_SELF,
    AXIS_ATTRIBUTE,
    AXIS_CHILD,
    AXIS_DESCENDANT,
    AXIS_DESCENDANT_OR_SELF,
    AXIS_FOLLOWING,
    AXIS_FOLLOWING_SIBLING,
    AXIS_NAMESPACE,
    AXIS_PARENT,
    AXIS_PRECEDING,
    AXIS_PRECEDING_SIBLING,
    AXIS_SELF
} xmlXPathAxisVal;

typedef struct {
    xmlChar *name;
    union {
        xmlChar *prefix;
        const xmlChar *uri;
    } ns;
} xmlXPathOpQName;

typedef struct _xmlXPathOp xmlXPathOp;
typedef xmlXPathOp *xmlXPathOpPtr;
struct _xmlXPathOp {
    unsigned char op;
    unsigned char mode;
    unsigned char predMode;
    unsigned char type; /* only used during compilation */
    int ch1; /* first child */
    int ch2; /* second child */
    int nbArgs;

    xmlXPathOpQName qname; /* for name tests, variables and functions */

    union {
        int boolean;

        double number;

        xmlChar *string;

        xmlXPathFunction func;

        struct {
            xmlXPathAxisVal axis;
            int typeMask;
        } step;
    } as;
};

struct _xmlXPathCompExpr {
    int nbStep;			/* Number of steps in this expression */
    int maxStep;		/* Maximum number of steps allocated */
    xmlXPathOp *steps;	/* ops for computation of this expression */
    int root;			/* index of root step in expression */
    int flags;
    int maxEvalDepth;
    xmlChar *expr;		/* the expression being computed */
    xmlDictPtr dict;		/* the dictionary to use if any */
#ifdef XPATH_STREAMING
    xmlPatternPtr stream;
#endif
};

/************************************************************************
 *									*
 *			Forward declarations				*
 *									*
 ************************************************************************/

static xmlNodePtr
xmlXPathNodeSetDupNs(xmlNodePtr node, xmlNsPtr ns);
static void
xmlXPathReleaseObject(xmlXPathContextPtr ctxt, xmlXPathObjectPtr obj);
static void
xmlXPathFreeObjectEntry(void *obj, const xmlChar *name);
static int
xmlXPathCompOpEval(xmlXPathContextPtr ctxt, xmlXPathItem *result,
                   int opIndex, xmlXPathEvalMode mode);

/************************************************************************
 *									*
 *			Parser Type functions				*
 *									*
 ************************************************************************/

/**
 * xmlXPathNewCompExpr:
 *
 * Create a new Xpath component
 *
 * Returns the newly allocated xmlXPathCompExprPtr or NULL in case of error
 */
static xmlXPathCompExprPtr
xmlXPathNewCompExpr(void) {
    xmlXPathCompExprPtr cur;

    cur = (xmlXPathCompExprPtr) xmlMalloc(sizeof(xmlXPathCompExpr));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlXPathCompExpr));

    return(cur);
}

/**
 * xmlXPathFreeCompExpr:
 * @comp:  an XPATH comp
 *
 * Free up the memory allocated by @comp
 */
void
xmlXPathFreeCompExpr(xmlXPathCompExprPtr comp)
{
    xmlXPathOpPtr op;
    int i;

    if (comp == NULL)
        return;
    for (i = 0; i < comp->nbStep; i++) {
        op = &comp->steps[i];

        switch (op->op) {
            case XPATH_OP_VALUE_STRING:
                xmlFree(op->as.string);
                break;

            case XPATH_OP_STEP:
            case XPATH_OP_STEP_CTXT:
            case XPATH_OP_VARIABLE:
                if ((comp->dict == NULL) && (op->qname.name != NULL)) {
                    xmlFree(op->qname.name);

                    if ((comp->flags & XML_XPATH_COMPILE_NS) == 0)
                        xmlFree(op->qname.ns.prefix);
                }
                break;

            case XPATH_OP_SFUNC:
            case XPATH_OP_FUNCTION:
                if ((comp->dict == NULL) && (op->qname.name != NULL)) {
                    xmlFree(op->qname.name);

                    /*
                     * The check for fptr is a bit tricky.
                     */
                    if (((comp->flags & XML_XPATH_COMPILE_NS) == 0) &&
                        (op->as.func == NULL))
                        xmlFree(op->qname.ns.prefix);
                }
                break;

            default:
                break;
        }
    }
    if (comp->dict != NULL) {
        xmlDictFree(comp->dict);
    }
    if (comp->steps != NULL) {
        xmlFree(comp->steps);
    }
#ifdef XPATH_STREAMING
    if (comp->stream != NULL) {
        xmlFreePatternList(comp->stream);
    }
#endif
    if (comp->expr != NULL) {
        xmlFree(comp->expr);
    }

    xmlFree(comp);
}

static int
xmlXPathCompOpSetQName(xmlXPathContextPtr ctxt, xmlXPathOpQName *qname,
                       xmlChar *name, xmlChar *prefix, const xmlChar *nsUri) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;

    if (comp->dict == NULL) {
        qname->name = name;

        if (nsUri != NULL) {
            qname->ns.uri = nsUri;
            xmlFree(prefix);
        } else {
            qname->ns.prefix = prefix;
        }
    } else {
        const xmlChar *dname;
        const xmlChar *dprefix;

        dname = xmlDictLookup(comp->dict, name, -1);
        if (dname == NULL) {
            xmlXPathErrMemory(ctxt);
            return(-1);
        }

        if (nsUri != NULL) {
            qname->ns.uri = nsUri;
        } else if (prefix != NULL) {
            dprefix = xmlDictLookup(comp->dict, prefix, -1);
            if (dprefix == NULL) {
                xmlXPathErrMemory(ctxt);
                return(-1);
            }
            qname->ns.prefix = (xmlChar *) dprefix;
        } else {
            qname->ns.prefix = NULL;
        }

        qname->name = (xmlChar *) dname;

        xmlFree(name);
        xmlFree(prefix);
    }

    return(0);
}

/**
 * xmlXPathCompOpSetEvalMode:
 * @ctxt:  parser context
 * @opIndex:  index in op table
 * @mode:  evaluation mode
 * @osPred:  whether the mode comes from a predicate
 *
 * Update the evaluation mode of an operation. Propagate the mode
 * to child ops if possible.
 */
static void
xmlXPathCompOpSetEvalMode(xmlXPathContextPtr ctxt, int opIndex,
                          xmlXPathEvalMode mode, int isPred) {
    xmlXPathOpPtr steps = ctxt->pctxt.comp->steps;
    xmlXPathOpPtr op = &steps[opIndex];
    int oldMode, isStep;

    /*
     * Mode applies to steps, filters, predicates, unions, sorts
     * and nodesets.
     */

    isStep = ((op->op == XPATH_OP_STEP) || (op->op == XPATH_OP_STEP_CTXT));

    if ((!isStep) &&
        (op->op != XPATH_OP_FILTER) &&
        (op->op != XPATH_OP_PREDICATE) &&
        (op->op != XPATH_OP_UNION) &&
        (op->op != XPATH_OP_SORT) &&
        (op->op != XPATH_OP_NODESET))
        return;

    if ((isPred) && (isStep))
        oldMode = op->predMode;
    else
        oldMode = op->mode;

    /* Shouldn't happen? */
    if (oldMode == XPATH_EVAL_ANY)
        return;

    if (oldMode == XPATH_EVAL_NONE)
        return;

    if ((mode != XPATH_EVAL_ANY) && (oldMode != XPATH_EVAL_ALL)) {
        /*
         * Both old and new mode select a single node
         */

        if ((mode == XPATH_EVAL_FIRST) || (mode == XPATH_EVAL_LAST)) {
            /* [n][1], [n][last()]: Ignore second filter. */
            return;
        } else {
            /*
             * [n][2]: Can't match
             *
             * We could propagate XPATH_EVAL_NONE and eliminate
             * whole operations but this shouldn't be a common case.
             */
            mode = XPATH_EVAL_NONE;
        }
    }

    /*
     * Step ops need two modes. One for inner proecessing and
     * one for the whole step op after merging and sorting.
     */
    if ((isPred) && (isStep)) {
        op->predMode = mode;
    } else {
        op->mode = mode;
    }

    /*
     * Some modes can be propagated to child ops.
     *
     * There's no benefit from setting modes on NODESET children.
     */
    if ((mode == XPATH_EVAL_ANY) ||
        (mode == XPATH_EVAL_FIRST) ||
        (mode == XPATH_EVAL_LAST)) {
        if (op->op == XPATH_OP_UNION) {
            xmlXPathOpPtr childOp1 = &steps[op->ch1];
            xmlXPathOpPtr childOp2 = &steps[op->ch2];

            if (childOp1->op != XPATH_OP_NODESET)
                xmlXPathCompOpSetEvalMode(ctxt, op->ch1, mode, 0);
            if (childOp2->op != XPATH_OP_NODESET)
                xmlXPathCompOpSetEvalMode(ctxt, op->ch2, mode, 0);
        } else if (op->op == XPATH_OP_SORT) {
            xmlXPathOpPtr childOp = &steps[op->ch1];

            if (childOp->op != XPATH_OP_NODESET)
                xmlXPathCompOpSetEvalMode(ctxt, op->ch1, mode, 0);
        } else if ((!isPred) && (isStep)) {
            int predIndex;

            /*
             * Step ops can propagate the outer mode to the inner
             * mode or to the last predicate which is ch2.
             *
             * For reverse axes, first and last must be flipped.
             */

            if (op->ch2 == -1)
                predIndex = opIndex;
            else
                predIndex = op->ch2;

            if (AXIS_IS_REVERSE(op->as.step.axis)) {
                if (mode == XPATH_EVAL_FIRST)
                    mode = XPATH_EVAL_LAST;
                else if (mode == XPATH_EVAL_LAST)
                    mode = XPATH_EVAL_FIRST;
            }

            xmlXPathCompOpSetEvalMode(ctxt, predIndex, mode, 1);
        }
    }
}

/**
 * xmlXPathCompAdd:
 * @ctxt:  parser context
 * @opcode:  operation code
 * @retType:  the return type
 *
 * Add an operation to a compiled expression.
 *
 * Returns the op or NULL in case of failure.
 */
static int
xmlXPathCompAdd(xmlXPathContextPtr ctxt, xmlXPathOp **opPtr,
                xmlXPathOpcode opcode, xmlXPathObjectType retType) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathOpPtr op;

    if (comp->nbStep >= comp->maxStep) {
	xmlXPathOp *tmp;
        int newSize;

        newSize = xmlGrowCapacity(comp->maxStep, sizeof(tmp[0]),
                                  10, XPATH_MAX_STEPS);
        if (newSize < 0) {
	    xmlXPathErrMemory(ctxt);
	    return(-1);
        }
	tmp = xmlRealloc(comp->steps, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlXPathErrMemory(ctxt);
	    return(-1);
	}
	comp->steps = tmp;
	comp->maxStep = newSize;
    }

    op = &comp->steps[comp->nbStep];
    op->op = opcode;
    op->mode = 0;
    op->type = retType;
    op->ch1 = -1;
    op->ch2 = -1;

    if (opPtr != NULL)
        *opPtr = op;

    return(comp->nbStep++);
}

static int
xmlXPathCompAddUnary(xmlXPathContextPtr ctxt, xmlXPathOp **opPtr,
                     xmlXPathOpcode opcode, xmlXPathObjectType retType,
                     int ch1) {
    xmlXPathOpPtr op;
    int opIndex;

    opIndex = xmlXPathCompAdd(ctxt, &op, opcode, retType);
    if (opIndex < 0)
        return(opIndex);

    op->ch1 = ch1;

    if (opPtr != NULL)
        *opPtr = op;

    return(opIndex);
}

static int
xmlXPathCompAddBinary(xmlXPathContextPtr ctxt, xmlXPathOp **opPtr,
                      xmlXPathOpcode opcode, xmlXPathObjectType retType,
                      int ch1, int ch2) {
    xmlXPathOpPtr op;
    int opIndex;

    opIndex = xmlXPathCompAdd(ctxt, &op, opcode, retType);
    if (opIndex < 0)
        return(opIndex);

    op->ch1 = ch1;
    op->ch2 = ch2;

    if (opPtr != NULL)
        *opPtr = op;

    return(opIndex);
}

/**
 * xmlXPathCompGetArg:
 * @ctxt:  parser context
 * @type:  expected type
 *
 * Process an argument to another op. Create type conversion ops
 * if necessary. Set evaluation mode on arg op.
 *
 * Returns the index of final argument operation.
 */
static int
xmlXPathCompGetArg(xmlXPathContextPtr ctxt, int argIndex,
                   xmlXPathObjectType type) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathOpPtr op;

    op = &comp->steps[argIndex];

    switch (type) {
        /*
         * TODO: create casts depending on source type
         */
        case XPATH_BOOLEAN:
            if (op->type != XPATH_BOOLEAN) {
                xmlXPathCompOpSetEvalMode(ctxt, argIndex, XPATH_EVAL_ANY, 0);

                argIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_BOOL,
                                                XPATH_BOOLEAN, argIndex);
            }
            break;
        case XPATH_NUMBER:
            if (op->type != XPATH_NUMBER) {
                xmlXPathCompOpSetEvalMode(ctxt, argIndex, XPATH_EVAL_FIRST, 0);

                argIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_NUMBER,
                                                XPATH_NUMBER, argIndex);
            }
            break;
        case XPATH_STRING:
            if (op->type != XPATH_STRING) {
                xmlXPathCompOpSetEvalMode(ctxt, argIndex, XPATH_EVAL_FIRST, 0);

                argIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_STRING,
                                                XPATH_STRING, argIndex);
            }
            break;
        case XPATH_NODESET:
            if (op->type != XPATH_NODESET) {
                if (op->type != XPATH_UNDEFINED) {
                    xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
                    return(-1);
                }
                argIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_NODESET,
                                                XPATH_NODESET, argIndex);
            }
            break;
        case XPATH_XSLT_TREE:
            if ((op->type != XPATH_NODESET) && (op->type != XPATH_XSLT_TREE)) {
                if (op->type != XPATH_UNDEFINED) {
                    xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
                    return(-1);
                }
                argIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_XSLT_TREE,
                                                XPATH_NODESET, argIndex);
            }
            break;
        case XPATH_UNDEFINED:
        default:
            break;
    }

    return(argIndex);
}

static int
xmlXPathCompAddSort(xmlXPathContextPtr ctxt, int opIndex) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathOpPtr op;

    if (opIndex < 0)
        return(-1);

    op = &comp->steps[opIndex];

    if ((op->op == XPATH_OP_UNION) ||
        (op->op == XPATH_OP_STEP) ||
        ((op->op == XPATH_OP_STEP_CTXT) &&
         (AXIS_IS_REVERSE(op->as.step.axis))) ||
        (op->op == XPATH_OP_FUNCTION) ||
        ((op->op == XPATH_OP_SFUNC) &&
         (op->type == XPATH_NODESET)) ||
        ((op->op == XPATH_OP_NODESET) &&
         (comp->steps[op->ch1].op == XPATH_OP_FUNCTION))) {
        return(xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_SORT, op->type,
                                    opIndex));
    } else {
        return(opIndex);
    }
}

static int
xmlXPathCompAddStep(xmlXPathContextPtr ctxt, xmlXPathOp **opPtr,
                    int argIndex, int predIndex,
                    xmlXPathAxisVal axis, int typeMask) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathOpPtr op, argOp;
    xmlXPathOpcode opcode;
    int opIndex;

    argIndex = xmlXPathCompGetArg(ctxt, argIndex, XPATH_NODESET);
    if (argIndex < 0)
        return(argIndex);

    argOp = &comp->steps[argIndex];
    if (argOp->op == XPATH_OP_NODE) {
        opcode = XPATH_OP_STEP_CTXT;
        argIndex = -1;
    } else {
        opcode = XPATH_OP_STEP;
    }

    opIndex = xmlXPathCompAdd(ctxt, &op, opcode, XPATH_NODESET);
    if (opIndex < 0)
        return(opIndex);

    op->ch1 = argIndex;
    op->ch2 = predIndex;
    op->predMode = 0;
    op->as.step.axis = axis;
    op->as.step.typeMask = typeMask;
    op->qname.name = NULL;
    op->qname.ns.prefix = NULL;

    if (opPtr != NULL)
        *opPtr = op;

    return(opIndex);
}

/************************************************************************
 *									*
 *		XPath object cache structures				*
 *									*
 ************************************************************************/

/* #define XP_DEFAULT_CACHE_ON */

typedef struct _xmlXPathContextCache xmlXPathContextCache;
typedef xmlXPathContextCache *xmlXPathContextCachePtr;
struct _xmlXPathContextCache {
    xmlNodePtr *nodeArrays;         /* first element points to next */
    int numNodeArrays;
    int maxNodeArrays;
    xmlXPathObjectPtr nodesetObjs;  /* stringval points to next */
    int numNodeset;
    int maxNodeset;
    xmlXPathObjectPtr miscObjs;     /* stringval points to next */
    int numMisc;
    int maxMisc;
};

/************************************************************************
 *									*
 *		Debugging related functions				*
 *									*
 ************************************************************************/

#ifdef LIBXML_DEBUG_ENABLED
static void
xmlXPathDebugDumpNode(FILE *output, xmlNodePtr cur, int depth) {
    int i;
    char shift[100];

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;
    if (cur == NULL) {
	fprintf(output, "%s", shift);
	fprintf(output, "Node is NULL !\n");
	return;

    }

    if ((cur->type == XML_DOCUMENT_NODE) ||
	     (cur->type == XML_HTML_DOCUMENT_NODE)) {
	fprintf(output, "%s", shift);
	fprintf(output, " /\n");
    } else if (cur->type == XML_ATTRIBUTE_NODE)
	xmlDebugDumpAttr(output, (xmlAttrPtr)cur, depth);
    else
	xmlDebugDumpOneNode(output, cur, depth);
}
static void
xmlXPathDebugDumpNodeList(FILE *output, xmlNodePtr cur, int depth) {
    xmlNodePtr tmp;
    int i;
    char shift[100];

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;
    if (cur == NULL) {
	fprintf(output, "%s", shift);
	fprintf(output, "Node is NULL !\n");
	return;

    }

    while (cur != NULL) {
	tmp = cur;
	cur = cur->next;
	xmlDebugDumpOneNode(output, tmp, depth);
    }
}

static void
xmlXPathDebugDumpNodeSet(FILE *output, xmlNodeSetPtr cur, int depth) {
    int i;
    char shift[100];

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;

    if (cur == NULL) {
	fprintf(output, "%s", shift);
	fprintf(output, "NodeSet is NULL !\n");
	return;

    }

    if (cur != NULL) {
	fprintf(output, "Set contains %d nodes:\n", cur->nodeNr);
	for (i = 0;i < cur->nodeNr;i++) {
	    fprintf(output, "%s", shift);
	    fprintf(output, "%d", i + 1);
	    xmlXPathDebugDumpNode(output, cur->nodeTab[i], depth + 1);
	}
    }
}

static void
xmlXPathDebugDumpValueTree(FILE *output, xmlNodeSetPtr cur, int depth) {
    int i;
    char shift[100];

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;

    if ((cur == NULL) || (cur->nodeNr == 0) || (cur->nodeTab[0] == NULL)) {
	fprintf(output, "%s", shift);
	fprintf(output, "Value Tree is NULL !\n");
	return;

    }

    fprintf(output, "%s", shift);
    fprintf(output, "%d", i + 1);
    xmlXPathDebugDumpNodeList(output, cur->nodeTab[0]->children, depth + 1);
}

/**
 * xmlXPathDebugDumpObject:
 * @output:  the FILE * to dump the output
 * @cur:  the object to inspect
 * @depth:  indentation level
 *
 * Dump the content of the object for debugging purposes
 */
void
xmlXPathDebugDumpObject(FILE *output, xmlXPathObjectPtr cur, int depth) {
    int i;
    char shift[100];

    if (output == NULL) return;

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;


    fprintf(output, "%s", shift);

    if (cur == NULL) {
        fprintf(output, "Object is empty (NULL)\n");
	return;
    }
    switch(cur->type) {
        case XPATH_UNDEFINED:
	    fprintf(output, "Object is uninitialized\n");
	    break;
        case XPATH_NODESET:
	    fprintf(output, "Object is a Node Set :\n");
	    xmlXPathDebugDumpNodeSet(output, cur->nodesetval, depth);
	    break;
	case XPATH_XSLT_TREE:
	    fprintf(output, "Object is an XSLT value tree :\n");
	    xmlXPathDebugDumpValueTree(output, cur->nodesetval, depth);
	    break;
        case XPATH_BOOLEAN:
	    fprintf(output, "Object is a Boolean : ");
	    if (cur->boolval) fprintf(output, "true\n");
	    else fprintf(output, "false\n");
	    break;
        case XPATH_NUMBER:
	    switch (xmlXPathIsInf(cur->floatval)) {
	    case 1:
		fprintf(output, "Object is a number : Infinity\n");
		break;
	    case -1:
		fprintf(output, "Object is a number : -Infinity\n");
		break;
	    default:
		if (xmlXPathIsNaN(cur->floatval)) {
		    fprintf(output, "Object is a number : NaN\n");
		} else if (cur->floatval == 0) {
                    /* Omit sign for negative zero. */
		    fprintf(output, "Object is a number : 0\n");
		} else {
		    fprintf(output, "Object is a number : %0g\n", cur->floatval);
		}
	    }
	    break;
        case XPATH_STRING:
	    fprintf(output, "Object is a string : ");
	    xmlDebugDumpString(output, cur->stringval);
	    fprintf(output, "\n");
	    break;
	case XPATH_USERS:
	    fprintf(output, "Object is user defined\n");
	    break;
    }
}

static void
xmlXPathDebugDumpEvalMode(FILE *output, const char *name,
                          xmlXPathEvalMode mode) {
    switch (mode) {
        case XPATH_EVAL_NONE:
            fprintf(output, " %s=NONE", name);
            break;
        case XPATH_EVAL_ANY:
            fprintf(output, " %s=ANY", name);
            break;
        case XPATH_EVAL_FIRST:
            fprintf(output, " %s=FIRST", name);
            break;
        case XPATH_EVAL_LAST:
            fprintf(output, " %s=LAST", name);
            break;
        case XPATH_EVAL_ALL:
            break;
        default:
            fprintf(output, " %s=%d", name, mode);
            break;
    }
}

static void
xmlXPathDebugDumpStepOp(FILE *output, const xmlXPathCompExpr *comp,
                        const xmlXPathOp *op, int depth) {
    int i;
    char shift[100];

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;

    fprintf(output, "%s", shift);
    if (op == NULL) {
	fprintf(output, "Step is NULL\n");
	return;
    }
    switch (op->op) {
        case XPATH_OP_END:
	    fprintf(output, "END"); break;
        case XPATH_OP_BOOL:
	    fprintf(output, "BOOL"); break;
        case XPATH_OP_NUMBER:
	    fprintf(output, "NUMBER"); break;
        case XPATH_OP_STRING:
	    fprintf(output, "STRING"); break;
        case XPATH_OP_XSLT_TREE:
	    fprintf(output, "XSLT_TREE"); break;
        case XPATH_OP_NODESET:
	    fprintf(output, "NODESET"); break;
        case XPATH_OP_NOT:
	    fprintf(output, "NOT"); break;
        case XPATH_OP_AND:
	    fprintf(output, "AND"); break;
        case XPATH_OP_OR:
	    fprintf(output, "OR"); break;
        case XPATH_OP_EQ:
	    fprintf(output, "EQ"); break;
	case XPATH_OP_NE:
	    fprintf(output, "NE"); break;
        case XPATH_OP_EQ_NUM:
	    fprintf(output, "EQ_NUM"); break;
	case XPATH_OP_NE_NUM:
	    fprintf(output, "NE_NUM"); break;
        case XPATH_OP_EQ_STR:
	    fprintf(output, "EQ_STR"); break;
	case XPATH_OP_NE_STR:
	    fprintf(output, "NE_STR"); break;
        case XPATH_OP_LT:
	    fprintf(output, "LT"); break;
        case XPATH_OP_LE:
	    fprintf(output, "LE"); break;
        case XPATH_OP_LT_NUM:
	    fprintf(output, "LT_NUM"); break;
        case XPATH_OP_LE_NUM:
	    fprintf(output, "LE_NUM"); break;
        case XPATH_OP_NEG:
	    fprintf(output, "NEG"); break;
        case XPATH_OP_FLOOR:
	    fprintf(output, "FLOOR"); break;
        case XPATH_OP_CEIL:
	    fprintf(output, "CEIL"); break;
        case XPATH_OP_ROUND:
	    fprintf(output, "ROUND"); break;
        case XPATH_OP_ADD:
	    fprintf(output, "ADD"); break;
        case XPATH_OP_SUB:
	    fprintf(output, "SUB"); break;
        case XPATH_OP_MULT:
	    fprintf(output, "MULT"); break;
        case XPATH_OP_DIV:
	    fprintf(output, "DIV"); break;
        case XPATH_OP_MOD:
	    fprintf(output, "MOD"); break;
        case XPATH_OP_UNION:
	     fprintf(output, "UNION"); break;
        case XPATH_OP_ROOT:
	     fprintf(output, "ROOT"); break;
        case XPATH_OP_NODE:
	     fprintf(output, "NODE"); break;
        case XPATH_OP_SORT:
	     fprintf(output, "SORT"); break;
        case XPATH_OP_STEP:
	    fprintf(output, "STEP "); break;
        case XPATH_OP_STEP_CTXT:
	    fprintf(output, "STEP_CTXT "); break;
	case XPATH_OP_VALUE_BOOL:
	    fprintf(output, "VALUE_BOOL %s",
                    (op->as.boolean) ? "true" : "false");
            break;
	case XPATH_OP_VALUE_NUMBER:
	    fprintf(output, "VALUE_NUMBER %g", op->as.number);
            break;
	case XPATH_OP_VALUE_STRING:
	    fprintf(output, "VALUE_STRING %s", op->as.string);
            break;
	case XPATH_OP_VARIABLE: {
	    const xmlChar *prefix = op->qname.ns.prefix;
	    const xmlChar *name = op->qname.name;

	    if (prefix != NULL)
		fprintf(output, "VARIABLE %s:%s", prefix, name);
	    else
		fprintf(output, "VARIABLE %s", name);
	    break;
	}
        case XPATH_OP_SFUNC:
	case XPATH_OP_FUNCTION: {
	    int nbargs = op->nbArgs;
	    const xmlChar *prefix = op->qname.ns.prefix;
	    const xmlChar *name = op->qname.name;

            if (prefix != NULL)
		fprintf(output, "FUNCTION %s:%s (%d args)",
			prefix, name, nbargs);
	    else
		fprintf(output, "FUNCTION %s (%d args)", name, nbargs);
	    break;
	}
        case XPATH_OP_ARG:
            fprintf(output, "ARG"); break;
        case XPATH_OP_PREDICATE:
            fprintf(output, "PREDICATE"); break;
        case XPATH_OP_FILTER:
            fprintf(output, "FILTER"); break;
        case XPATH_OP_POSITION:
            fprintf(output, "POSITION"); break;
        case XPATH_OP_LAST:
            fprintf(output, "LAST"); break;
        case XPATH_OP_LOCAL_NAME:
            fprintf(output, "LOCAL_NAME"); break;
        case XPATH_OP_LOCAL_NAME_CTXT:
            fprintf(output, "LOCAL_NAME_CTXT"); break;
        case XPATH_OP_NAME:
            fprintf(output, "NAME"); break;
        case XPATH_OP_NAME_CTXT:
            fprintf(output, "NAME_CTXT"); break;
        case XPATH_OP_NAMESPACE_URI:
            fprintf(output, "NAMESPACE_URI"); break;
        case XPATH_OP_NAMESPACE_URI_CTXT:
            fprintf(output, "NAMESPACE_URI_CTXT"); break;
	default:
            fprintf(output, "UNKNOWN %d", op->op); return;
    }

    switch (op->op) {
        case XPATH_OP_STEP:
        case XPATH_OP_STEP_CTXT: {
	    xmlXPathAxisVal axis = op->as.step.axis;
	    int type = op->as.step.typeMask;
	    const xmlChar *prefix = op->qname.ns.prefix;
	    const xmlChar *name = op->qname.name;

	    switch (axis) {
		case AXIS_ANCESTOR:
		    fprintf(output, "ancestor::"); break;
		case AXIS_ANCESTOR_OR_SELF:
		    fprintf(output, "ancestor-or-self::"); break;
		case AXIS_ATTRIBUTE:
		    fprintf(output, "attribute::"); break;
		case AXIS_CHILD:
		    fprintf(output, "child::"); break;
		case AXIS_DESCENDANT:
		    fprintf(output, "descendant::"); break;
		case AXIS_DESCENDANT_OR_SELF:
		    fprintf(output, "descendant-or-self::"); break;
		case AXIS_FOLLOWING:
		    fprintf(output, "following::"); break;
		case AXIS_FOLLOWING_SIBLING:
		    fprintf(output, "following-sibling::"); break;
		case AXIS_NAMESPACE:
		    fprintf(output, "namespace::"); break;
		case AXIS_PARENT:
		    fprintf(output, "parent::"); break;
		case AXIS_PRECEDING:
		    fprintf(output, "preceding::"); break;
		case AXIS_PRECEDING_SIBLING:
		    fprintf(output, "preceding-sibling::"); break;
		case AXIS_SELF:
		    fprintf(output, "self::"); break;
	    }
            switch (type) {
                case TYPE_MASK_NODE:
                    fprintf(output, "node()"); break;
                case TYPE_MASK_TEXT:
                    fprintf(output, "text()"); break;
                case TYPE_MASK_COMMENT:
                    fprintf(output, "comment()"); break;
                case TYPE_MASK_PI:
                    fprintf(output, "processing-instruction()"); break;
                default:
                    if (prefix != NULL)
                        fprintf(output, "%s:", prefix);
                    if (name != NULL)
                        fprintf(output, "%s", name);
                    else
                        fprintf(output, "*");
                    break;
            }
	    break;

        }
        default:
            break;
    }

    switch (op->op) {
        case XPATH_OP_STEP:
        case XPATH_OP_STEP_CTXT:
        case XPATH_OP_FILTER:
        case XPATH_OP_PREDICATE:
        case XPATH_OP_UNION:
        case XPATH_OP_SORT:
        case XPATH_OP_NODESET:
            xmlXPathDebugDumpEvalMode(output, "mode", op->mode);
            break;
        default:
            break;
    }

    if ((op->op == XPATH_OP_STEP) || (op->op == XPATH_OP_STEP_CTXT))
        xmlXPathDebugDumpEvalMode(output, "predMode", op->predMode);

    fprintf(output, "\n");

    if (op->ch1 >= 0)
	xmlXPathDebugDumpStepOp(output, comp, &comp->steps[op->ch1], depth + 1);
    if (op->ch2 >= 0)
	xmlXPathDebugDumpStepOp(output, comp, &comp->steps[op->ch2], depth + 1);
}

/**
 * xmlXPathDebugDumpCompExpr:
 * @output:  the FILE * for the output
 * @comp:  the precompiled XPath expression
 * @depth:  the indentation level.
 *
 * Dumps the tree of the compiled XPath expression.
 */
void
xmlXPathDebugDumpCompExpr(FILE *output, xmlXPathCompExprPtr comp,
	                  int depth) {
    int i;
    char shift[100];

    if ((output == NULL) || (comp == NULL)) return;

    for (i = 0;((i < depth) && (i < 25));i++)
        shift[2 * i] = shift[2 * i + 1] = ' ';
    shift[2 * i] = shift[2 * i + 1] = 0;

    fprintf(output, "%s", shift);

#ifdef XPATH_STREAMING
    if (comp->stream) {
        fprintf(output, "Streaming Expression\n");
    } else
#endif
    {
        fprintf(output,
                "Compiled Expression: nbStep = %d, maxEvalDepth = %d\n",
                comp->nbStep, comp->maxEvalDepth);
        i = comp->root;
        xmlXPathDebugDumpStepOp(output, comp, &comp->steps[i], depth + 1);
    }
}

#endif /* LIBXML_DEBUG_ENABLED */

/************************************************************************
 *									*
 *			XPath object caching				*
 *									*
 ************************************************************************/

/**
 * xmlXPathNewCache:
 *
 * Create a new object cache
 *
 * Returns the xmlXPathCache just allocated.
 */
static xmlXPathContextCachePtr
xmlXPathNewCache(void)
{
    xmlXPathContextCachePtr ret;

    ret = (xmlXPathContextCachePtr) xmlMalloc(sizeof(xmlXPathContextCache));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathContextCache));
    ret->maxNodeArrays = 100;
    ret->maxNodeset = 20;
    ret->maxMisc = 20;
    return(ret);
}

static void
xmlXPathFreeCache(xmlXPathContextCachePtr cache)
{
    xmlNodePtr *nodes;
    xmlXPathObjectPtr obj;

    if (cache == NULL)
	return;

    nodes = cache->nodeArrays;
    while (nodes != NULL) {
        xmlNodePtr *next = (void *) nodes[0];

        xmlFree(nodes);
        nodes = next;
    }

    obj = cache->nodesetObjs;
    while (obj != NULL) {
        xmlXPathObjectPtr next = (void *) obj->stringval;

	xmlFree(obj->nodesetval);
	xmlFree(obj);

        obj = next;
    }

    obj = cache->miscObjs;
    while (obj != NULL) {
        xmlXPathObjectPtr next = (void *) obj->stringval;

	xmlFree(obj);

        obj = next;
    }

    xmlFree(cache);
}

/**
 * xmlXPathContextSetCache:
 *
 * @ctxt:  the XPath context
 * @active: enables/disables (creates/frees) the cache
 * @value: a value with semantics dependent on @options
 * @options: options (currently only the value 0 is used)
 *
 * Creates/frees an object cache on the XPath context.
 * If activates XPath objects (xmlXPathObject) will be cached internally
 * to be reused.
 * @options:
 *   0: This will set the XPath object caching:
 *      @value:
 *        This will set the maximum number of XPath objects
 *        to be cached per slot
 *        There are two slots for node-set and misc objects.
 *        Use <0 for the default number (100).
 *   Other values for @options have currently no effect.
 *
 * Returns 0 if the setting succeeded, and -1 on API or internal errors.
 */
int
xmlXPathContextSetCache(xmlXPathContextPtr ctxt,
			int active,
			int value,
			int options)
{
    if (ctxt == NULL)
	return(-1);
    if (active) {
	xmlXPathContextCachePtr cache;

	if (ctxt->cache == NULL) {
	    ctxt->cache = xmlXPathNewCache();
	    if (ctxt->cache == NULL) {
                xmlXPathErrMemory(ctxt);
		return(-1);
            }
	}
	cache = (xmlXPathContextCachePtr) ctxt->cache;
	if (options == 0) {
	    if (value < 0)
		value = 100;
            cache->maxNodeArrays = value;
	}
    } else if (ctxt->cache != NULL) {
	xmlXPathFreeCache((xmlXPathContextCachePtr) ctxt->cache);
	ctxt->cache = NULL;
    }
    return(0);
}

static xmlXPathObjectPtr
xmlXPathCacheNewMisc(xmlXPathContextPtr ctxt) {
    xmlXPathObjectPtr ret;
    xmlXPathContextCachePtr cache = ctxt->cache;

    if ((cache != NULL) && (cache->miscObjs != NULL)) {
        ret = cache->miscObjs;
        cache->miscObjs = (void *) ret->stringval;
        cache->numMisc -= 1;
        return(ret);
    }

    ret = xmlMalloc(sizeof(*ret));
    if (ret == NULL)
        xmlXPathErrMemory(ctxt);

    return(ret);
}

/**
 * xmlXPathCacheWrapString:
 * @ctxt:  the XPath context
 * @val:  the xmlChar * value
 *
 * This is the cached version of xmlXPathWrapString().
 * Wraps the @val string into an XPath object.
 *
 * Returns the created or reused object.
 */
static xmlXPathObjectPtr
xmlXPathCacheWrapString(xmlXPathContextPtr ctxt, xmlChar *val) {
    xmlXPathObjectPtr ret;

    ret = xmlXPathCacheNewMisc(ctxt);
    if (ret == NULL) {
        xmlFree(val);
        return(NULL);
    }

    ret->type = XPATH_STRING;
    ret->stringval = val;
    return(ret);
}

/**
 * xmlXPathCacheNewNodeSet:
 * @ctxt:  the XPath context
 *
 * This is the cached version of xmlXPathNewNodeSet().
 *
 * Returns the created or reused object.
 */
static xmlXPathObjectPtr
xmlXPathCacheNewNodeSet(xmlXPathContextPtr ctxt) {
    xmlXPathContextCachePtr cache = ctxt->cache;
    xmlXPathObjectPtr ret;

    if ((cache != NULL) && (cache->nodesetObjs != NULL)) {
        /*
        * Use the nodeset-cache.
        */
        ret = cache->nodesetObjs;
        cache->nodesetObjs = (void *) ret->stringval;
        cache->numNodeset -= 1;
    } else {
        xmlNodeSetPtr set;

        /*
        * Fallback to misc-cache.
        */
        ret = xmlXPathCacheNewMisc(ctxt);
        if (ret == NULL)
            return(NULL);

        set = xmlMalloc(sizeof(*set));
        if (set == NULL) {
            xmlXPathErrMemory(ctxt);
            xmlFree(ret);
            return(NULL);
        }

        ret->nodesetval = set;
    }

    ret->type = XPATH_NODESET;
    ret->boolval = 0;
    ret->nodesetval->nodeTab = NULL;
    ret->nodesetval->nodeNr = 0;
    ret->nodesetval->nodeMax = 0;

    return(ret);
}

/**
 * xmlXPathCacheNewString:
 * @ctxt:  the XPath context
 * @val:  the xmlChar * value
 *
 * This is the cached version of xmlXPathNewString().
 * Acquire an xmlXPathObjectPtr of type string and of value @val
 *
 * Returns the created or reused object.
 */
static xmlXPathObjectPtr
xmlXPathCacheNewString(xmlXPathContextPtr ctxt, const xmlChar *val) {
    xmlXPathObjectPtr ret;
    xmlChar *copy;

    if (val == NULL)
        val = BAD_CAST "";

    copy = xmlStrdup(val);
    if (copy == NULL) {
        xmlXPathErrMemory(ctxt);
        return(NULL);
    }

    ret = xmlXPathCacheNewMisc(ctxt);
    if (ret == NULL) {
        xmlFree(copy);
        return(NULL);
    }

    ret->type = XPATH_STRING;
    ret->stringval = copy;
    return(ret);
}

/**
 * xmlXPathCacheNewCString:
 * @ctxt:  the XPath context
 * @val:  the char * value
 *
 * This is the cached version of xmlXPathNewCString().
 * Acquire an xmlXPathObjectPtr of type string and of value @val
 *
 * Returns the created or reused object.
 */
static xmlXPathObjectPtr
xmlXPathCacheNewCString(xmlXPathContextPtr ctxt, const char *val) {
    return xmlXPathCacheNewString(ctxt, BAD_CAST val);
}

/**
 * xmlXPathCacheNewBoolean:
 * @ctxt:  the XPath context
 * @val:  the boolean value
 *
 * This is the cached version of xmlXPathNewBoolean().
 * Acquires an xmlXPathObjectPtr of type boolean and of value @val
 *
 * Returns the created or reused object.
 */
static xmlXPathObjectPtr
xmlXPathCacheNewBoolean(xmlXPathContextPtr ctxt, int val) {
    xmlXPathObjectPtr ret;

    ret = xmlXPathCacheNewMisc(ctxt);
    if (ret == NULL)
        return(NULL);

    ret->type = XPATH_BOOLEAN;
    ret->boolval = (val != 0);
    return(ret);
}

/**
 * xmlXPathCacheNewFloat:
 * @ctxt the XPath context
 * @val:  the double value
 *
 * This is the cached version of xmlXPathNewFloat().
 * Acquires an xmlXPathObjectPtr of type double and of value @val
 *
 * Returns the created or reused object.
 */
static xmlXPathObjectPtr
xmlXPathCacheNewFloat(xmlXPathContextPtr ctxt, double val) {
    xmlXPathObjectPtr ret;

    ret = xmlXPathCacheNewMisc(ctxt);
    if (ret == NULL)
        return(NULL);

    ret->type = XPATH_NUMBER;
    ret->floatval = val;
    return(ret);
}

static xmlXPathObjectPtr
xmlXPathCacheNewExternal(xmlXPathContextPtr ctxt, void *user) {
    xmlXPathObjectPtr ret;

    ret = xmlXPathCacheNewMisc(ctxt);
    if (ret == NULL)
        return(NULL);

    ret->type = XPATH_USERS;
    ret->user = user;
    return(ret);
}

static int
xmlXPathCacheNodeSetGrow(xmlXPathContextPtr ctxt, xmlNodeSetPtr set) {
    xmlXPathContextCachePtr cache;
    xmlNodePtr *nodes;
    int newSize;

    cache = ctxt->cache;

    if ((set->nodeMax == 0) &&
        (cache != NULL) &&
        (cache->nodeArrays != NULL)) {
        nodes = cache->nodeArrays;
        cache->nodeArrays = (void *) nodes[0];
        cache->numNodeArrays -= 1;

        newSize = XML_NODESET_DEFAULT;
    } else {
        newSize = xmlGrowCapacity(set->nodeMax, sizeof(nodes[0]),
                                  XML_NODESET_DEFAULT, XPATH_MAX_NODESET_LENGTH);
        if (newSize < 0) {
            xmlXPathErrMemory(ctxt);
            return(-1);
        }

        if ((set->nodeMax == XML_NODESET_DEFAULT) &&
            (cache != NULL) &&
            (cache->numNodeArrays < cache->maxNodeArrays)) {
            nodes = xmlMalloc(newSize * sizeof(nodes[0]));
            if (nodes == NULL) {
                xmlXPathErrMemory(ctxt);
                return(-1);
            }

            memcpy(nodes, set->nodeTab, set->nodeNr * sizeof(nodes[0]));

            set->nodeTab[0] = (void *) cache->nodeArrays;
            cache->nodeArrays = set->nodeTab;
            cache->numNodeArrays += 1;
        } else {
            nodes = xmlRealloc(set->nodeTab, newSize * sizeof(nodes[0]));
            if (nodes == NULL) {
                xmlXPathErrMemory(ctxt);
                return(-1);
            }
        }
    }

    set->nodeMax = newSize;
    set->nodeTab = nodes;

    return(0);
}

static int
xmlXPathCacheNodeSetAdd(xmlXPathContextPtr ctxt, xmlNodeSetPtr set,
                        xmlNodePtr val) {
    if (set->nodeNr >= set->nodeMax) {
        if (xmlXPathCacheNodeSetGrow(ctxt, set) < 0)
            return(-1);
    }

    set->nodeTab[set->nodeNr++] = val;

    return(0);
}

static int
xmlXPathCacheNodeSetCopy(xmlXPathContextPtr ctxt, xmlNodeSetPtr dst,
                         xmlNodeSetPtr src) {
    int hasNsNodes;
    int i;

    if (src->nodeNr <= XML_NODESET_DEFAULT) {
        if (xmlXPathCacheNodeSetGrow(ctxt, dst) < 0)
            return(-1);
    } else {
        xmlNodePtr *tmp;

        tmp = xmlMalloc(src->nodeNr * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlXPathErrMemory(ctxt);
            return(-1);
        }

        dst->nodeTab = tmp;
        dst->nodeMax = src->nodeNr;
    }

    hasNsNodes = 0;
    for (i = 0; i < src->nodeNr; i++) {
        xmlNodePtr node = src->nodeTab[i];

        if (node->type == XML_NAMESPACE_DECL) {
            xmlNsPtr ns = (xmlNsPtr) node;

            node = xmlXPathNodeSetDupNs((xmlNodePtr) ns->next, ns);
            if (node == NULL) {
                dst->nodeNr = i;
                xmlXPathNodeSetClear(dst, hasNsNodes);
                xmlFree(dst->nodeTab);
                dst->nodeTab = NULL;
                dst->nodeMax = 0;
                return(-1);
            }

            hasNsNodes = 1;
        }

        dst->nodeTab[i] = node;
    }

    dst->nodeNr = src->nodeNr;

    return(hasNsNodes);
}

/************************************************************************
 *									*
 *		Parser stacks related functions and macros		*
 *									*
 ************************************************************************/

/**
 * xmlXPathCastToNumberInternal:
 * @ctxt:  parser context
 * @val:  an XPath object
 *
 * Converts an XPath object to its number value
 *
 * Returns the number value
 */
static double
xmlXPathCastToNumberInternal(xmlXPathContextPtr ctxt,
                             xmlXPathObjectPtr val) {
    double ret = 0.0;

    if (val == NULL)
	return(xmlXPathNAN);
    switch (val->type) {
    case XPATH_UNDEFINED:
	ret = xmlXPathNAN;
	break;
    case XPATH_NODESET:
    case XPATH_XSLT_TREE: {
        xmlChar *str;

	str = xmlXPathCastNodeSetToString(val->nodesetval);
        if (str == NULL) {
            xmlXPathErrMemory(ctxt);
            ret = xmlXPathNAN;
        } else {
	    ret = xmlXPathCastStringToNumber(str);
            xmlFree(str);
        }
	break;
    }
    case XPATH_STRING:
	ret = xmlXPathCastStringToNumber(val->stringval);
	break;
    case XPATH_NUMBER:
	ret = val->floatval;
	break;
    case XPATH_BOOLEAN:
	ret = xmlXPathCastBooleanToNumber(val->boolval);
	break;
    case XPATH_USERS:
	/* TODO */
	ret = xmlXPathNAN;
	break;
    }
    return(ret);
}

static int
xmlXPathGrowValueTable(xmlXPathContextPtr ctxt) {
    xmlXPathObjectPtr *tmp;
    int newSize;

    newSize = xmlGrowCapacity(ctxt->pctxt.valueMax, sizeof(tmp[0]),
                              10, XPATH_MAX_STACK_DEPTH);
    if (newSize < 0) {
        xmlXPathErrMemory(ctxt);
        return(-1);
    }
    tmp = xmlRealloc(ctxt->pctxt.valueTab, newSize * sizeof(tmp[0]));
    if (tmp == NULL) {
        xmlXPathErrMemory(ctxt);
        return(-1);
    }
    ctxt->pctxt.valueTab = tmp;
    ctxt->pctxt.valueMax = newSize;

    return(0);
}

static void
xmlXPathValuePushInternal(xmlXPathContextPtr ctxt, xmlXPathObjectPtr value) {
    if (ctxt->pctxt.valueNr >= ctxt->pctxt.valueMax) {
        if (xmlXPathGrowValueTable(ctxt) < 0) {
            xmlXPathFreeObject(value);
            return;
        }
    }

    ctxt->pctxt.valueTab[ctxt->pctxt.valueNr++] = value;
    ctxt->pctxt.value = value;
}

static xmlXPathObjectPtr
xmlXPathValuePopInternal(xmlXPathContextPtr ctxt) {
    ctxt->pctxt.valueNr--;

    if (ctxt->pctxt.valueNr > 0)
        ctxt->pctxt.value = ctxt->pctxt.valueTab[ctxt->pctxt.valueNr - 1];
    else
        ctxt->pctxt.value = NULL;

    return(ctxt->pctxt.valueTab[ctxt->pctxt.valueNr]);
}

/**
 * valuePop:
 * @ctxt: an XPath evaluation context
 *
 * Pops the top XPath object from the value stack
 *
 * Returns the XPath object just removed
 */
xmlXPathObjectPtr
valuePop(xmlXPathParserContextPtr ctxt)
{
    xmlXPathObjectPtr ret;

    if ((ctxt == NULL) || (ctxt->valueNr <= 0))
        return (NULL);

    ctxt->valueNr--;
    if (ctxt->valueNr > 0)
        ctxt->value = ctxt->valueTab[ctxt->valueNr - 1];
    else
        ctxt->value = NULL;
    ret = ctxt->valueTab[ctxt->valueNr];
    return (ret);
}

/**
 * valuePush:
 * @ctxt:  an XPath evaluation context
 * @value:  the XPath object
 *
 * Pushes a new XPath object on top of the value stack. If value is NULL,
 * a memory error is recorded in the parser context.
 *
 * Returns the number of items on the value stack, or -1 in case of error.
 *
 * The object is destroyed in case of error.
 */
int
valuePush(xmlXPathParserContextPtr ctxt, xmlXPathObjectPtr value)
{
    if (ctxt == NULL) return(-1);
    if (value == NULL) {
        /*
         * A NULL value typically indicates that a memory allocation failed.
         */
        xmlXPathErrMemory(ctxt->context);
        return(-1);
    }
    if (ctxt->valueNr >= ctxt->valueMax) {
        if (xmlXPathGrowValueTable(ctxt->context) < 0) {
            xmlXPathFreeObject(value);
            return(-1);
        }
    }
    ctxt->valueTab[ctxt->valueNr] = value;
    ctxt->value = value;
    return (ctxt->valueNr++);
}

/**
 * xmlXPathPopBoolean:
 * @ctxt:  an XPath parser context
 *
 * Pops a boolean from the stack, handling conversion if needed.
 * Check error with #xmlXPathCheckError.
 *
 * Returns the boolean
 */
int
xmlXPathPopBoolean(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr obj;
    int ret;

    if (ctxt == NULL)
        return(0);
    xpctxt = ctxt->context;

    obj = valuePop(ctxt);
    if (obj == NULL) {
	xmlXPathCErr(xpctxt, XPATH_STACK_ERROR);
	return(0);
    }
    if (obj->type != XPATH_BOOLEAN)
	ret = xmlXPathCastToBoolean(obj);
    else
        ret = obj->boolval;
    xmlXPathReleaseObject(xpctxt, obj);
    return(ret);
}

/**
 * xmlXPathPopNumber:
 * @ctxt:  an XPath parser context
 *
 * Pops a number from the stack, handling conversion if needed.
 * Check error with #xmlXPathCheckError.
 *
 * Returns the number
 */
double
xmlXPathPopNumber(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr obj;
    double ret;

    if (ctxt == NULL)
        return(0);
    xpctxt = ctxt->context;

    obj = valuePop(ctxt);
    if (obj == NULL) {
	xmlXPathCErr(xpctxt, XPATH_STACK_ERROR);
	return(0);
    }
    if (obj->type != XPATH_NUMBER)
	ret = xmlXPathCastToNumberInternal(xpctxt, obj);
    else
        ret = obj->floatval;
    xmlXPathReleaseObject(xpctxt, obj);
    return(ret);
}

/**
 * xmlXPathPopString:
 * @ctxt:  an XPath parser context
 *
 * Pops a string from the stack, handling conversion if needed.
 * Check error with #xmlXPathCheckError.
 *
 * Returns the string
 */
xmlChar *
xmlXPathPopString(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr obj;
    xmlChar *ret;

    if (ctxt == NULL)
        return(0);
    xpctxt = ctxt->context;

    obj = valuePop(ctxt);
    if (obj == NULL) {
	xmlXPathCErr(xpctxt, XPATH_STACK_ERROR);
	return(NULL);
    }
    if (obj->type != XPATH_STRING) {
        ret = xmlXPathCastToString(obj);
        if (ret == NULL)
            xmlXPathErrMemory(xpctxt);
    } else {
        ret = obj->stringval;
        obj->stringval = NULL;
    }
    xmlXPathReleaseObject(xpctxt, obj);
    return(ret);
}

/**
 * xmlXPathPopNodeSet:
 * @ctxt:  an XPath parser context
 *
 * Pops a node-set from the stack, handling conversion if needed.
 * Check error with #xmlXPathCheckError.
 *
 * Returns the node-set
 */
xmlNodeSetPtr
xmlXPathPopNodeSet(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr obj;
    xmlNodeSetPtr ret;

    if (ctxt == NULL)
        return(0);
    xpctxt = ctxt->context;

    obj = valuePop(ctxt);
    if (obj == NULL) {
	xmlXPathCErr(xpctxt, XPATH_STACK_ERROR);
	return(NULL);
    }
    if (obj->type != XPATH_NODESET) {
	xmlXPathCErr(xpctxt, XPATH_INVALID_TYPE);
	return(NULL);
    }
    ret = obj->nodesetval;
    obj->nodesetval = NULL;
    xmlXPathReleaseObject(xpctxt, obj);
    return(ret);
}

/**
 * xmlXPathPopExternal:
 * @ctxt:  an XPath parser context
 *
 * Pops an external object from the stack, handling conversion if needed.
 * Check error with #xmlXPathCheckError.
 *
 * Returns the object
 */
void *
xmlXPathPopExternal(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr obj;
    void *ret;

    if (ctxt == NULL)
        return(0);
    xpctxt = ctxt->context;

    obj = valuePop(ctxt);
    if (obj == NULL) {
	xmlXPathCErr(xpctxt, XPATH_STACK_ERROR);
	return(NULL);
    }
    if (obj->type != XPATH_USERS) {
	xmlXPathCErr(xpctxt, XPATH_INVALID_TYPE);
	return(NULL);
    }
    ret = obj->user;
    obj->user = NULL;
    xmlXPathReleaseObject(xpctxt, obj);
    return(ret);
}

/*
 * Macros for accessing the content. Those should be used only by the parser,
 * and not exported.
 *
 * Dirty macros, i.e. one need to make assumption on the context to use them
 *
 *   CUR_PTR return the current pointer to the xmlChar to be parsed.
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

#define CUR (*ctxt->pctxt.cur)
#define SKIP(val) ctxt->pctxt.cur += (val)
#define NXT(val) ctxt->pctxt.cur[(val)]
#define CUR_PTR ctxt->pctxt.cur
#define CUR_CHAR(l) xmlXPathCurrentChar(ctxt, &l)

#define COPY_BUF(b, i, v)						\
    if (v < 0x80) b[i++] = v;						\
    else i += xmlCopyCharMultiByte(&b[i],v)

#define NEXTL(l)  ctxt->pctxt.cur += l

#define SKIP_BLANKS							\
    while (IS_BLANK_CH(*(ctxt->pctxt.cur))) NEXT

#define CURRENT (*ctxt->pctxt.cur)
#define NEXT ((*ctxt->pctxt.cur) ?  ctxt->pctxt.cur++: ctxt->pctxt.cur)


#ifndef DBL_DIG
#define DBL_DIG 16
#endif
#ifndef DBL_EPSILON
#define DBL_EPSILON 1E-9
#endif

#define UPPER_DOUBLE 1E9
#define LOWER_DOUBLE 1E-5
#define	LOWER_DOUBLE_EXP 5

#define INTEGER_DIGITS DBL_DIG
#define FRACTION_DIGITS (DBL_DIG + 1 + (LOWER_DOUBLE_EXP))
#define EXPONENT_DIGITS (3 + 2)

/**
 * xmlXPathFormatNumber:
 * @number:     number to format
 * @buffer:     output buffer
 * @buffersize: size of output buffer
 *
 * Convert the number into a string representation.
 */
static void
xmlXPathFormatNumber(double number, char buffer[], int buffersize)
{
    switch (xmlXPathIsInf(number)) {
    case 1:
	if (buffersize > (int)sizeof("Infinity"))
	    snprintf(buffer, buffersize, "Infinity");
	break;
    case -1:
	if (buffersize > (int)sizeof("-Infinity"))
	    snprintf(buffer, buffersize, "-Infinity");
	break;
    default:
	if (xmlXPathIsNaN(number)) {
	    if (buffersize > (int)sizeof("NaN"))
		snprintf(buffer, buffersize, "NaN");
	} else if (number == 0) {
            /* Omit sign for negative zero. */
	    snprintf(buffer, buffersize, "0");
	} else if ((number > INT_MIN) && (number < INT_MAX) &&
                   (number == (int) number)) {
	    char work[30];
	    char *ptr, *cur;
	    int value = (int) number;

            ptr = &buffer[0];
	    if (value == 0) {
		*ptr++ = '0';
	    } else {
		snprintf(work, 29, "%d", value);
		cur = &work[0];
		while ((*cur) && (ptr - buffer < buffersize)) {
		    *ptr++ = *cur++;
		}
	    }
	    if (ptr - buffer < buffersize) {
		*ptr = 0;
	    } else if (buffersize > 0) {
		ptr--;
		*ptr = 0;
	    }
	} else {
	    /*
	      For the dimension of work,
	          DBL_DIG is number of significant digits
		  EXPONENT is only needed for "scientific notation"
	          3 is sign, decimal point, and terminating zero
		  LOWER_DOUBLE_EXP is max number of leading zeroes in fraction
	      Note that this dimension is slightly (a few characters)
	      larger than actually necessary.
	    */
	    char work[DBL_DIG + EXPONENT_DIGITS + 3 + LOWER_DOUBLE_EXP];
	    int integer_place, fraction_place;
	    char *ptr;
	    char *after_fraction;
	    double absolute_value;
	    int size;

	    absolute_value = fabs(number);

	    /*
	     * First choose format - scientific or regular floating point.
	     * In either case, result is in work, and after_fraction points
	     * just past the fractional part.
	    */
	    if ( ((absolute_value > UPPER_DOUBLE) ||
		  (absolute_value < LOWER_DOUBLE)) &&
		 (absolute_value != 0.0) ) {
		/* Use scientific notation */
		integer_place = DBL_DIG + EXPONENT_DIGITS + 1;
		fraction_place = DBL_DIG - 1;
		size = snprintf(work, sizeof(work),"%*.*e",
			 integer_place, fraction_place, number);
		while ((size > 0) && (work[size] != 'e')) size--;

	    }
	    else {
		/* Use regular notation */
		if (absolute_value > 0.0) {
		    integer_place = (int)log10(absolute_value);
		    if (integer_place > 0)
		        fraction_place = DBL_DIG - integer_place - 1;
		    else
		        fraction_place = DBL_DIG - integer_place;
		} else {
		    fraction_place = 1;
		}
		size = snprintf(work, sizeof(work), "%0.*f",
				fraction_place, number);
	    }

	    /* Remove leading spaces sometimes inserted by snprintf */
	    while (work[0] == ' ') {
	        for (ptr = &work[0];(ptr[0] = ptr[1]);ptr++);
		size--;
	    }

	    /* Remove fractional trailing zeroes */
	    after_fraction = work + size;
	    ptr = after_fraction;
	    while (*(--ptr) == '0')
		;
	    if (*ptr != '.')
	        ptr++;
	    while ((*ptr++ = *after_fraction++) != 0);

	    /* Finally copy result back to caller */
	    size = strlen(work) + 1;
	    if (size > buffersize) {
		work[buffersize - 1] = 0;
		size = buffersize;
	    }
	    memmove(buffer, work, size);
	}
	break;
    }
}


/************************************************************************
 *									*
 *			Routines to handle NodeSets			*
 *									*
 ************************************************************************/

/**
 * xmlXPathOrderDocElems:
 * @doc:  an input document
 *
 * Call this routine to speed up XPath computation on static documents.
 * This stamps all the element nodes with the document order
 * Like for line information, the order is kept in the element->content
 * field, the value stored is actually - the node number (starting at -1)
 * to be able to differentiate from line numbers.
 *
 * Returns the number of elements found in the document or -1 in case
 *    of error.
 */
long
xmlXPathOrderDocElems(xmlDocPtr doc) {
    XML_INTPTR_T count = 0;
    xmlNodePtr cur;

    if (doc == NULL)
	return(-1);
    cur = doc->children;
    while (cur != NULL) {
	if (cur->type == XML_ELEMENT_NODE) {
            count += 1;
            cur->content = XML_INT_TO_PTR(-count);
	    if (cur->children != NULL) {
		cur = cur->children;
		continue;
	    }
	}
	if (cur->next != NULL) {
	    cur = cur->next;
	    continue;
	}
	do {
	    cur = cur->parent;
	    if (cur == NULL)
		break;
	    if (cur == (xmlNodePtr) doc) {
		cur = NULL;
		break;
	    }
	    if (cur->next != NULL) {
		cur = cur->next;
		break;
	    }
	} while (cur != NULL);
    }
    return(count);
}

/**
 * xmlXPathCmpNodes:
 * @node1:  the first node
 * @node2:  the second node
 *
 * Compare two nodes w.r.t document order
 *
 * Returns -2 in case of error 1 if first point < second point, 0 if
 *         it's the same node, -1 otherwise
 */
int
xmlXPathCmpNodes(xmlNodePtr node1, xmlNodePtr node2) {
    int depth1, depth2;
    int precedence1 = 0, precedence2 = 0;
    xmlNodePtr miscNode1 = NULL, miscNode2 = NULL;
    xmlNodePtr cur, root;

    if ((node1 == NULL) || (node2 == NULL))
	return(-2);
    /*
     * a couple of optimizations which will avoid computations in most cases
     */
    if (node1 == node2)		/* trivial case */
	return(0);
    if (node1->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node1;

        precedence1 = 1;
        miscNode1 = node1;
        node1 = (xmlNodePtr) ns->next;
    } else if (node1->type == XML_ATTRIBUTE_NODE) {
        precedence1 = 2;
	miscNode1 = node1;
	node1 = node1->parent;
    }
    if (node2->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node2;

        precedence2 = 1;
        miscNode2 = node2;
        node2 = (xmlNodePtr) ns->next;
    } else if (node2->type == XML_ATTRIBUTE_NODE) {
	precedence2 = 2;
	miscNode2 = node2;
	node2 = node2->parent;
    }
    if (node1 == node2) {
	if (precedence1 == precedence2) {
            if (precedence1 == 1) {
                xmlNsPtr ns1 = (xmlNsPtr) miscNode1;
                xmlNsPtr ns2 = (xmlNsPtr) miscNode2;

                return(xmlStrcmp(ns2->prefix, ns1->prefix));
            } else {
	        cur = miscNode2->prev;
		while (cur != NULL) {
		    if (cur == miscNode1)
		        return (1);
		    cur = cur->prev;
		}
		return (-1);
	    }
	    return(0);
	} else if (precedence1 < precedence2) {
            return(1);
        } else {
            return(-1);
        }
    }
    if (node1 == node2->prev)
	return(1);
    if (node1 == node2->next)
	return(-1);

    /*
     * Speedup using document order if available.
     */
    if ((node1->type == XML_ELEMENT_NODE) &&
	(node2->type == XML_ELEMENT_NODE) &&
	(0 > XML_NODE_SORT_VALUE(node1)) &&
	(0 > XML_NODE_SORT_VALUE(node2)) &&
	(node1->doc == node2->doc)) {
	XML_INTPTR_T l1, l2;

	l1 = -XML_NODE_SORT_VALUE(node1);
	l2 = -XML_NODE_SORT_VALUE(node2);
	if (l1 < l2)
	    return(1);
	if (l1 > l2)
	    return(-1);
    }

    /*
     * compute depth to root
     */
    for (depth2 = 0, cur = node2;cur->parent != NULL;cur = cur->parent) {
	if (cur->parent == node1)
	    return(1);
	depth2++;
    }
    root = cur;
    for (depth1 = 0, cur = node1;cur->parent != NULL;cur = cur->parent) {
	if (cur->parent == node2)
	    return(-1);
	depth1++;
    }
    /*
     * Distinct document (or distinct entities :-( ) case.
     */
    if (root != cur) {
	return(-2);
    }
    /*
     * get the nearest common ancestor.
     */
    while (depth1 > depth2) {
	depth1--;
	node1 = node1->parent;
    }
    while (depth2 > depth1) {
	depth2--;
	node2 = node2->parent;
    }
    while (node1->parent != node2->parent) {
	node1 = node1->parent;
	node2 = node2->parent;
	/* should not happen but just in case ... */
	if ((node1 == NULL) || (node2 == NULL))
	    return(-2);
    }
    /*
     * Find who's first.
     */
    if (node1 == node2->prev)
	return(1);
    if (node1 == node2->next)
	return(-1);
    /*
     * Speedup using document order if available.
     */
    if ((node1->type == XML_ELEMENT_NODE) &&
	(node2->type == XML_ELEMENT_NODE) &&
	(0 > XML_NODE_SORT_VALUE(node1)) &&
	(0 > XML_NODE_SORT_VALUE(node2)) &&
	(node1->doc == node2->doc)) {
	XML_INTPTR_T l1, l2;

	l1 = -XML_NODE_SORT_VALUE(node1);
	l2 = -XML_NODE_SORT_VALUE(node2);
	if (l1 < l2)
	    return(1);
	if (l1 > l2)
	    return(-1);
    }

    for (cur = node1->next;cur != NULL;cur = cur->next)
	if (cur == node2)
	    return(1);
    return(-1); /* assume there is no sibling list corruption */
}

#ifndef WITH_TIM_SORT
static int
xmlXPathCmpNodesQSort(const void *v1, const void *v2) {
    xmlNode *const *node1 = v1;
    xmlNode *const *node2 = v2;

#ifdef XP_OPTIMIZED_NON_ELEM_COMPARISON
    return(xmlXPathCmpNodesExt(*node2, *node1));
#else
    return(xmlXPathCmpNodes(*node2, *node1));
#endif
}
#endif

/**
 * xmlXPathNodeSetSort:
 * @set:  the node set
 *
 * Sort the node set in document order
 */
void
xmlXPathNodeSetSort(xmlNodeSetPtr set) {
    if (set == NULL)
	return;

#ifndef WITH_TIM_SORT
    qsort(set->nodeTab, set->nodeNr, sizeof(set->nodeTab[0]),
          xmlXPathCmpNodesQSort);
#else /* WITH_TIM_SORT */
    libxml_domnode_tim_sort(set->nodeTab, set->nodeNr);
#endif /* WITH_TIM_SORT */
}

static void
xmlXPathNodeSetKeep(xmlXPathItem *item, int index) {
    int i;

    if (item->hasNsNodes) {
        /* Free other nodes */
        for (i = 0; i < item->as.nodeset.nodeNr; i++) {
            if (i != index) {
                xmlNodePtr cur = item->as.nodeset.nodeTab[i];

                if ((cur != NULL) && (cur->type == XML_NAMESPACE_DECL))
                    xmlXPathNodeSetFreeNs((xmlNsPtr) cur);
            }
        }
    }

    if (index < item->as.nodeset.nodeNr) {
        item->as.nodeset.nodeTab[0] = item->as.nodeset.nodeTab[index];
        item->as.nodeset.nodeNr = 1;
    } else {
        item->as.nodeset.nodeNr = 0;
    }
}

/**
 * xmlXPathNodeSetFindFirst:
 * @set: the node set to be filtered
 * @mode: evaluation mode
 *
 * Find the first or last node in document order and remove
 * all other nodes.
 */
static void
xmlXPathNodeSetFindFirst(xmlXPathItem *item, xmlXPathEvalMode mode) {
    xmlNodePtr node;
    int wanted, index, i;

    wanted = (mode == XPATH_EVAL_FIRST) ? -1 : 1;
    index = 0;
    node = item->as.nodeset.nodeTab[0];
    for (i = 1; i < item->as.nodeset.nodeNr; i++) {
        xmlNodePtr other = item->as.nodeset.nodeTab[i];
        int res;

        res = xmlXPathCmpNodes(node, other);
        if (res == wanted) {
            index = i;
            node = other;
        }
    }

    xmlXPathNodeSetKeep(item, index);
}

/**
 * xmlXPathNodeSetFinish:
 * @set: the node set to be filtered
 * @mode: evaluation mode
 *
 * Remove duplicates and pick nodes according to evaluation mode.
 */
static void
xmlXPathNodeSetFinish(xmlXPathItem *item, xmlXPathEvalMode mode) {
    xmlNodePtr prev;
    int i, j;
    int pos = 0;

    /*
     * This is only called for XPATH_EVAL_ALL and index predicates
     * larger than 1.
     */
    if (mode == XPATH_EVAL_ALL) {
        if (item->as.nodeset.nodeNr < 2)
            return;
    } else {
        pos = mode;
        if (pos > item->as.nodeset.nodeNr) {
            xmlXPathNodeSetClear(&item->as.nodeset, item->hasNsNodes);
            return;
        }
    }

    /*
     * Remove duplicates
     */
    prev = item->as.nodeset.nodeTab[0];
    j = 1;
    for (i = 1; i < item->as.nodeset.nodeNr; i++) {
        xmlNodePtr node = item->as.nodeset.nodeTab[i];

        if (node == prev)
            continue;

        if ((node->type == XML_NAMESPACE_DECL) &&
            (prev->type == XML_NAMESPACE_DECL)) {
            xmlNsPtr ns = (xmlNsPtr) node;
            xmlNsPtr prevNs = (xmlNsPtr) prev;

            if ((ns->next == prevNs->next) &&
                (xmlStrEqual(ns->prefix, prevNs->prefix))) {
                xmlFreeNs(ns);
                item->as.nodeset.nodeTab[i] = NULL;
                continue;
            }
        }

        if (pos == 0) {
            item->as.nodeset.nodeTab[j++] = node;
        } else {
            j += 1;
            if (j == pos)
                break;
        }

        prev = node;
    }

    if (mode == XPATH_EVAL_ALL)
        item->as.nodeset.nodeNr = j;
    else
        xmlXPathNodeSetKeep(item, i);
}

/**
 * xmlXPathNodeSetDupNs:
 * @node:  the parent node of the namespace XPath node
 * @ns:  the libxml namespace declaration node.
 *
 * Namespace node in libxml don't match the XPath semantic. In a node set
 * the namespace nodes are duplicated and the next pointer is set to the
 * parent node in the XPath semantic.
 *
 * Returns the newly created object.
 */
static xmlNodePtr
xmlXPathNodeSetDupNs(xmlNodePtr node, xmlNsPtr ns) {
    xmlNsPtr cur;

    if ((ns == NULL) || (ns->type != XML_NAMESPACE_DECL))
	return(NULL);
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
	return((xmlNodePtr) ns);

    /*
     * Allocate a new Namespace and fill the fields.
     */
    cur = (xmlNsPtr) xmlMalloc(sizeof(xmlNs));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNs));
    cur->type = XML_NAMESPACE_DECL;
    if (ns->href != NULL) {
	cur->href = xmlStrdup(ns->href);
        if (cur->href == NULL) {
            xmlFree(cur);
            return(NULL);
        }
    }
    if (ns->prefix != NULL) {
	cur->prefix = xmlStrdup(ns->prefix);
        if (cur->prefix == NULL) {
            xmlFree((xmlChar *) cur->href);
            xmlFree(cur);
            return(NULL);
        }
    }
    cur->next = (xmlNsPtr) node;
    return((xmlNodePtr) cur);
}

/**
 * xmlXPathNodeSetFreeNs:
 * @ns:  the XPath namespace node found in a nodeset.
 *
 * Namespace nodes in libxml don't match the XPath semantic. In a node set
 * the namespace nodes are duplicated and the next pointer is set to the
 * parent node in the XPath semantic. Check if such a node needs to be freed
 */
void
xmlXPathNodeSetFreeNs(xmlNsPtr ns) {
    if ((ns == NULL) || (ns->type != XML_NAMESPACE_DECL))
	return;

    if ((ns->next != NULL) && (ns->next->type != XML_NAMESPACE_DECL)) {
	if (ns->href != NULL)
	    xmlFree((xmlChar *)ns->href);
	if (ns->prefix != NULL)
	    xmlFree((xmlChar *)ns->prefix);
	xmlFree(ns);
    }
}

static int
xmlXPathNodeSetAddInternal(xmlNodeSetPtr set, xmlNodePtr val) {
    if (val->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) val;
        val = xmlXPathNodeSetDupNs((xmlNodePtr) ns->next, ns);

        if (val == NULL)
            return(-1);
    }

    set->nodeTab[set->nodeNr++] = val;

    return(0);
}

/**
 * xmlXPathNodeSetCreate:
 * @val:  an initial xmlNodePtr, or NULL
 *
 * Create a new xmlNodeSetPtr of type double and of value @val
 *
 * Returns the newly created object.
 */
xmlNodeSetPtr
xmlXPathNodeSetCreate(xmlNodePtr val) {
    xmlNodeSetPtr ret;

    ret = (xmlNodeSetPtr) xmlMalloc(sizeof(xmlNodeSet));
    if (ret == NULL)
	return(NULL);
    ret->nodeNr = 0;

    if (val == NULL) {
        ret->nodeTab = NULL;
        ret->nodeMax = 0;
    } else {
        ret->nodeTab = (xmlNodePtr *) xmlMalloc(XML_NODESET_DEFAULT *
					     sizeof(xmlNodePtr));
	if (ret->nodeTab == NULL) {
	    xmlFree(ret);
	    return(NULL);
	}
	memset(ret->nodeTab, 0 ,
	       XML_NODESET_DEFAULT * sizeof(xmlNodePtr));
        ret->nodeMax = XML_NODESET_DEFAULT;

        if (xmlXPathNodeSetAddInternal(ret, val) < 0) {
            xmlXPathFreeNodeSet(ret);
            return(NULL);
        }
    }
    return(ret);
}

/**
 * xmlXPathNodeSetContains:
 * @cur:  the node-set
 * @val:  the node
 *
 * checks whether @cur contains @val
 *
 * Returns true (1) if @cur contains @val, false (0) otherwise
 */
int
xmlXPathNodeSetContains (xmlNodeSetPtr cur, xmlNodePtr val) {
    int i;

    if ((cur == NULL) || (val == NULL)) return(0);
    if (val->type == XML_NAMESPACE_DECL) {
	for (i = 0; i < cur->nodeNr; i++) {
	    if (cur->nodeTab[i]->type == XML_NAMESPACE_DECL) {
		xmlNsPtr ns1, ns2;

		ns1 = (xmlNsPtr) val;
		ns2 = (xmlNsPtr) cur->nodeTab[i];
		if (ns1 == ns2)
		    return(1);
		if ((ns1->next != NULL) && (ns2->next == ns1->next) &&
	            (xmlStrEqual(ns1->prefix, ns2->prefix)))
		    return(1);
	    }
	}
    } else {
	for (i = 0; i < cur->nodeNr; i++) {
	    if (cur->nodeTab[i] == val)
		return(1);
	}
    }
    return(0);
}

static int
xmlXPathNodeSetGrow(xmlNodeSetPtr cur) {
    xmlNodePtr *temp;
    int newSize;

    newSize = xmlGrowCapacity(cur->nodeMax, sizeof(temp[0]),
                              XML_NODESET_DEFAULT, XPATH_MAX_NODESET_LENGTH);
    if (newSize < 0)
        return(-1);
    temp = xmlRealloc(cur->nodeTab, newSize * sizeof(temp[0]));
    if (temp == NULL)
        return(-1);
    cur->nodeMax = newSize;
    cur->nodeTab = temp;

    return(0);
}

/**
 * xmlXPathNodeSetAddNs:
 * @cur:  the initial node set
 * @node:  the hosting node
 * @ns:  a the namespace node
 *
 * add a new namespace node to an existing NodeSet
 *
 * Returns 0 in case of success and -1 in case of error
 */
int
xmlXPathNodeSetAddNs(xmlNodeSetPtr cur, xmlNodePtr node, xmlNsPtr ns) {
    int i;
    xmlNodePtr nsNode;

    if ((cur == NULL) || (ns == NULL) || (node == NULL) ||
        (ns->type != XML_NAMESPACE_DECL) ||
	(node->type != XML_ELEMENT_NODE))
	return(-1);

    /* @@ with_ns to check whether namespace nodes should be looked at @@ */
    /*
     * prevent duplicates
     */
    for (i = 0;i < cur->nodeNr;i++) {
        if ((cur->nodeTab[i] != NULL) &&
	    (cur->nodeTab[i]->type == XML_NAMESPACE_DECL) &&
	    (((xmlNsPtr)cur->nodeTab[i])->next == (xmlNsPtr) node) &&
	    (xmlStrEqual(ns->prefix, ((xmlNsPtr)cur->nodeTab[i])->prefix)))
	    return(0);
    }

    /*
     * grow the nodeTab if needed
     */
    if (cur->nodeNr >= cur->nodeMax) {
        if (xmlXPathNodeSetGrow(cur) < 0)
            return(-1);
    }
    nsNode = xmlXPathNodeSetDupNs(node, ns);
    if(nsNode == NULL)
        return(-1);
    cur->nodeTab[cur->nodeNr++] = nsNode;
    return(0);
}

/**
 * xmlXPathNodeSetAdd:
 * @cur:  the initial node set
 * @val:  a new xmlNodePtr
 *
 * add a new xmlNodePtr to an existing NodeSet
 *
 * Returns 0 in case of success, and -1 in case of error
 */
int
xmlXPathNodeSetAdd(xmlNodeSetPtr cur, xmlNodePtr val) {
    int i;

    if ((cur == NULL) || (val == NULL)) return(-1);

    /* @@ with_ns to check whether namespace nodes should be looked at @@ */
    /*
     * prevent duplicates
     */
    for (i = 0;i < cur->nodeNr;i++)
        if (cur->nodeTab[i] == val) return(0);

    return(xmlXPathNodeSetAddUnique(cur, val));
}

/**
 * xmlXPathNodeSetAddUnique:
 * @cur:  the initial node set
 * @val:  a new xmlNodePtr
 *
 * add a new xmlNodePtr to an existing NodeSet, optimized version
 * when we are sure the node is not already in the set.
 *
 * Returns 0 in case of success and -1 in case of failure
 */
int
xmlXPathNodeSetAddUnique(xmlNodeSetPtr cur, xmlNodePtr val) {
    if ((cur == NULL) || (val == NULL)) return(-1);

    /* @@ with_ns to check whether namespace nodes should be looked at @@ */
    /*
     * grow the nodeTab if needed
     */
    if (cur->nodeNr >= cur->nodeMax) {
        if (xmlXPathNodeSetGrow(cur) < 0)
            return(-1);
    }

    return(xmlXPathNodeSetAddInternal(cur, val));
}

static xmlNodeSetPtr
xmlXPathNodeSetCopy(xmlNodeSetPtr set) {
    xmlNodeSetPtr result;
    xmlNodePtr *tmp = NULL;
    int size, i;

    result = xmlMalloc(sizeof(*result));
    if (result == NULL)
        return(NULL);

    size = set->nodeNr;
    if (size > 0) {
        tmp = xmlMalloc(size * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlFree(result);
            return(NULL);
        }
    }

    result->nodeNr = 0;
    result->nodeMax = size;
    result->nodeTab = tmp;

    for (i = 0; i < size; i++) {
        if (xmlXPathNodeSetAddInternal(result, set->nodeTab[i]) < 0) {
            xmlXPathFreeNodeSet(result);
            return(NULL);
        }
    }

    return(result);
}

/**
 * xmlXPathNodeSetMerge:
 * @val1:  the first NodeSet or NULL
 * @val2:  the second NodeSet
 *
 * Merges two nodesets, all nodes from @val2 are added to @val1
 * if @val1 is NULL, a new set is created and copied from @val2
 *
 * Returns @val1 once extended or NULL in case of error.
 *
 * Frees @val1 in case of error.
 */
xmlNodeSetPtr
xmlXPathNodeSetMerge(xmlNodeSetPtr val1, xmlNodeSetPtr val2) {
    int i, j, initNr, skip;
    xmlNodePtr n1, n2;

    if (val2 == NULL)
        return(val1);
    if (val1 == NULL)
        return(xmlXPathNodeSetCopy(val2));

    /* @@ with_ns to check whether namespace nodes should be looked at @@ */
    initNr = val1->nodeNr;

    for (i = 0;i < val2->nodeNr;i++) {
	n2 = val2->nodeTab[i];
	/*
	 * check against duplicates
	 */
	skip = 0;
	for (j = 0; j < initNr; j++) {
	    n1 = val1->nodeTab[j];
	    if (n1 == n2) {
		skip = 1;
		break;
	    } else if ((n1->type == XML_NAMESPACE_DECL) &&
		       (n2->type == XML_NAMESPACE_DECL)) {
		if ((((xmlNsPtr) n1)->next == ((xmlNsPtr) n2)->next) &&
		    (xmlStrEqual(((xmlNsPtr) n1)->prefix,
			((xmlNsPtr) n2)->prefix)))
		{
		    skip = 1;
		    break;
		}
	    }
	}
	if (skip)
	    continue;

        if (xmlXPathNodeSetAddUnique(val1, n2) < 0)
            goto error;
    }

    return(val1);

error:
    xmlXPathFreeNodeSet(val1);
    return(NULL);
}

/**
 * xmlXPathNodeSetMergeAndClear:
 * @set1:  the first NodeSet or NULL
 * @set2:  the second NodeSet
 *
 * Merges two nodesets, all nodes from @set2 are added to @set1.
 * Does not check for duplicates. Clears set2.
 *
 * Assumes both sets are non-empty.
 *
 * Returns @set1 once extended or NULL in case of error.
 *
 * Frees @set1 in case of error.
 */
ATTRIBUTE_NO_INLINE
static int
xmlXPathNodeSetMergeAndClear(xmlXPathContextPtr ctxt,
                             xmlXPathItem *item1, xmlXPathItem *item2,
                             xmlXPathEvalMode mode) {
    xmlNodeSetPtr set1, set2;
    int ret = -1;

    if ((mode == XPATH_EVAL_ANY) || (mode == XPATH_EVAL_NONE))
        return(0);

    set1 = &item1->as.nodeset;
    set2 = &item2->as.nodeset;

    if ((mode == XPATH_EVAL_FIRST) || (mode == XPATH_EVAL_LAST)) {
        int swap, index1, index2;
        xmlNodePtr node1, node2;

        /*
         * Compare and swap first or last elements of node sets
         */

        if (mode == XPATH_EVAL_FIRST) {
            index1 = 0;
            index2 = 0;
        } else {
            index1 = set1->nodeNr - 1;
            index2 = set2->nodeNr - 1;
        }

        node1 = set1->nodeTab[index1];
        node2 = set2->nodeTab[index2];
        swap = ((mode == XPATH_EVAL_FIRST) ? -1 : 1);

        if (
#ifdef XP_OPTIMIZED_NON_ELEM_COMPARISON
             (xmlXPathCmpNodesExt(node1, node2))
#else
             (xmlXPathCmpNodes(node1, node2))
#endif
             == swap) {
            set1->nodeTab[index1] = node2;
            set2->nodeTab[index2] = node1;

            if (node2->type == XML_NAMESPACE_DECL)
                item1->hasNsNodes = 1;
        }

        ret = 0;
    } else {
        int newSize;

        if (set2->nodeNr > XPATH_MAX_NODESET_LENGTH - set1->nodeNr) {
            xmlXPathErrMemory(ctxt);
            goto error;
        }

        newSize = set1->nodeNr + set2->nodeNr;

        while (newSize > set1->nodeMax) {
            if (xmlXPathCacheNodeSetGrow(ctxt, set1) < 0) {
                xmlXPathErrMemory(ctxt);
                goto error;
            }
        }

        memcpy(set1->nodeTab + set1->nodeNr, set2->nodeTab,
               set2->nodeNr * sizeof(set2->nodeTab[0]));

        set1->nodeNr += set2->nodeNr;
        set2->nodeNr = 0;

        if (item2->hasNsNodes)
            item1->hasNsNodes = 1;

        return(0);
    }

error:
    xmlXPathNodeSetClear(&item2->as.nodeset, item2->hasNsNodes);
    return(ret);
}

/**
 * xmlXPathNodeSetDel:
 * @cur:  the initial node set
 * @val:  an xmlNodePtr
 *
 * Removes an xmlNodePtr from an existing NodeSet
 */
void
xmlXPathNodeSetDel(xmlNodeSetPtr cur, xmlNodePtr val) {
    int i;

    if (cur == NULL) return;
    if (val == NULL) return;

    /*
     * find node in nodeTab
     */
    for (i = 0;i < cur->nodeNr;i++)
        if (cur->nodeTab[i] == val) break;

    if (i >= cur->nodeNr) {	/* not found */
        return;
    }
    if ((cur->nodeTab[i] != NULL) &&
	(cur->nodeTab[i]->type == XML_NAMESPACE_DECL))
	xmlXPathNodeSetFreeNs((xmlNsPtr) cur->nodeTab[i]);
    cur->nodeNr--;
    for (;i < cur->nodeNr;i++)
        cur->nodeTab[i] = cur->nodeTab[i + 1];
    cur->nodeTab[cur->nodeNr] = NULL;
}

/**
 * xmlXPathNodeSetRemove:
 * @cur:  the initial node set
 * @val:  the index to remove
 *
 * DEPRECATED: Don't use.
 *
 * Removes an entry from an existing NodeSet list.
 */
void
xmlXPathNodeSetRemove(xmlNodeSetPtr cur, int val) {
    if (cur == NULL) return;
    if (val >= cur->nodeNr) return;
    if ((cur->nodeTab[val] != NULL) &&
	(cur->nodeTab[val]->type == XML_NAMESPACE_DECL))
	xmlXPathNodeSetFreeNs((xmlNsPtr) cur->nodeTab[val]);
    cur->nodeNr--;
    for (;val < cur->nodeNr;val++)
        cur->nodeTab[val] = cur->nodeTab[val + 1];
    cur->nodeTab[cur->nodeNr] = NULL;
}

/**
 * xmlXPathFreeNodeSet:
 * @obj:  the xmlNodeSetPtr to free
 *
 * Free the NodeSet compound (not the actual nodes !).
 */
void
xmlXPathFreeNodeSet(xmlNodeSetPtr obj) {
    if (obj == NULL) return;
    if (obj->nodeTab != NULL) {
	int i;

	/* @@ with_ns to check whether namespace nodes should be looked at @@ */
	for (i = 0;i < obj->nodeNr;i++)
	    if ((obj->nodeTab[i] != NULL) &&
		(obj->nodeTab[i]->type == XML_NAMESPACE_DECL))
		xmlXPathNodeSetFreeNs((xmlNsPtr) obj->nodeTab[i]);
	xmlFree(obj->nodeTab);
    }
    xmlFree(obj);
}

/**
 * xmlXPathNodeSetClear:
 * @set:  the node set to clear
 *
 * Clears the list from all temporary XPath objects (e.g. namespace nodes
 * are feed), but does *not* free the list itself. Sets the length of the
 * list to 0.
 */
void
xmlXPathNodeSetClear(xmlNodeSetPtr set, int hasNsNodes)
{
    if (set == NULL)
	return;
    if (hasNsNodes) {
	int i;
	xmlNodePtr node;

	for (i = 0; i < set->nodeNr; i++) {
	    node = set->nodeTab[i];
	    if ((node != NULL) &&
		(node->type == XML_NAMESPACE_DECL))
		xmlXPathNodeSetFreeNs((xmlNsPtr) node);
	}
    }

    set->nodeNr = 0;
}

/**
 * xmlXPathNewNodeSet:
 * @val:  the NodePtr value
 *
 * Create a new xmlXPathObjectPtr of type NodeSet and initialize
 * it with the single Node @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewNodeSet(xmlNodePtr val) {
    xmlXPathObjectPtr ret;

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_NODESET;
    ret->boolval = 0;
    ret->nodesetval = xmlXPathNodeSetCreate(val);
    if (ret->nodesetval == NULL) {
        xmlFree(ret);
        return(NULL);
    }
    /* @@ with_ns to check whether namespace nodes should be looked at @@ */
    return(ret);
}

/**
 * xmlXPathNewValueTree:
 * @val:  the NodePtr value
 *
 * Create a new xmlXPathObjectPtr of type Value Tree (XSLT) and initialize
 * it with the tree root @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewValueTree(xmlNodePtr val) {
    xmlXPathObjectPtr ret;

    ret = xmlXPathNewNodeSet(val);
    if (ret == NULL)
	return(NULL);
    ret->type = XPATH_XSLT_TREE;

    return(ret);
}

/**
 * xmlXPathNewNodeSetList:
 * @val:  an existing NodeSet
 *
 * DEPRECATED: Don't use.
 *
 * Create a new xmlXPathObjectPtr of type NodeSet and initialize
 * it with the Nodeset @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewNodeSetList(xmlNodeSetPtr val)
{
    xmlXPathObjectPtr ret;

    if (val == NULL)
        return(NULL);

    ret = xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
        return(NULL);
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_NODESET;
    ret->boolval = 0;
    ret->nodesetval = xmlXPathNodeSetCopy(val);
    if (ret->nodesetval == NULL) {
        xmlFree(ret);
        return(NULL);
    }

    return (ret);
}

/**
 * xmlXPathWrapNodeSet:
 * @val:  the NodePtr value
 *
 * Wrap the Nodeset @val in a new xmlXPathObjectPtr
 *
 * Returns the newly created object.
 *
 * In case of error the node set is destroyed and NULL is returned.
 */
xmlXPathObjectPtr
xmlXPathWrapNodeSet(xmlNodeSetPtr val) {
    xmlXPathObjectPtr ret;

    if (val == NULL)
        return(xmlXPathNewNodeSet(NULL));

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL) {
        xmlXPathFreeNodeSet(val);
	return(NULL);
    }
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_NODESET;
    ret->nodesetval = val;
    return(ret);
}

/**
 * xmlXPathFreeNodeSetList:
 * @obj:  an existing NodeSetList object
 *
 * Free up the xmlXPathObjectPtr @obj but don't deallocate the objects in
 * the list contrary to xmlXPathFreeObject().
 */
void
xmlXPathFreeNodeSetList(xmlXPathObjectPtr obj) {
    if (obj == NULL) return;
    xmlFree(obj);
}

/**
 * xmlXPathDifference:
 * @nodes1:  a node-set
 * @nodes2:  a node-set
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets difference() function:
 *    node-set set:difference (node-set, node-set)
 *
 * Returns the difference between the two node sets, or nodes1 if
 *         nodes2 is empty
 */
xmlNodeSetPtr
xmlXPathDifference (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    xmlNodeSetPtr ret;
    int i, l1;
    xmlNodePtr cur;

    if (xmlXPathNodeSetIsEmpty(nodes2))
	return(nodes1);

    ret = xmlXPathNodeSetCreate(NULL);
    if (ret == NULL)
        return(NULL);
    if (xmlXPathNodeSetIsEmpty(nodes1))
	return(ret);

    l1 = xmlXPathNodeSetGetLength(nodes1);

    for (i = 0; i < l1; i++) {
	cur = xmlXPathNodeSetItem(nodes1, i);
	if (!xmlXPathNodeSetContains(nodes2, cur)) {
	    if (xmlXPathNodeSetAddUnique(ret, cur) < 0) {
                xmlXPathFreeNodeSet(ret);
	        return(NULL);
            }
	}
    }
    return(ret);
}

/**
 * xmlXPathIntersection:
 * @nodes1:  a node-set
 * @nodes2:  a node-set
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets intersection() function:
 *    node-set set:intersection (node-set, node-set)
 *
 * Returns a node set comprising the nodes that are within both the
 *         node sets passed as arguments
 */
xmlNodeSetPtr
xmlXPathIntersection (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    xmlNodeSetPtr ret = xmlXPathNodeSetCreate(NULL);
    int i, l1;
    xmlNodePtr cur;

    if (ret == NULL)
        return(ret);
    if (xmlXPathNodeSetIsEmpty(nodes1))
	return(ret);
    if (xmlXPathNodeSetIsEmpty(nodes2))
	return(ret);

    l1 = xmlXPathNodeSetGetLength(nodes1);

    for (i = 0; i < l1; i++) {
	cur = xmlXPathNodeSetItem(nodes1, i);
	if (xmlXPathNodeSetContains(nodes2, cur)) {
	    if (xmlXPathNodeSetAddUnique(ret, cur) < 0) {
                xmlXPathFreeNodeSet(ret);
	        return(NULL);
            }
	}
    }
    return(ret);
}

/**
 * xmlXPathDistinctSorted:
 * @nodes:  a node-set, sorted by document order
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets distinct() function:
 *    node-set set:distinct (node-set)
 *
 * Returns a subset of the nodes contained in @nodes, or @nodes if
 *         it is empty
 */
xmlNodeSetPtr
xmlXPathDistinctSorted (xmlNodeSetPtr nodes) {
    xmlNodeSetPtr ret;
    xmlHashTablePtr hash;
    int i, l;
    xmlChar * strval;
    xmlNodePtr cur;

    if (xmlXPathNodeSetIsEmpty(nodes))
	return(nodes);

    ret = xmlXPathNodeSetCreate(NULL);
    if (ret == NULL)
        return(ret);
    l = xmlXPathNodeSetGetLength(nodes);
    hash = xmlHashCreate (l);
    for (i = 0; i < l; i++) {
	cur = xmlXPathNodeSetItem(nodes, i);
	strval = xmlXPathCastNodeToString(cur);
	if (xmlHashLookup(hash, strval) == NULL) {
	    if (xmlHashAddEntry(hash, strval, strval) < 0) {
                xmlFree(strval);
                goto error;
            }
	    if (xmlXPathNodeSetAddUnique(ret, cur) < 0)
	        goto error;
	} else {
	    xmlFree(strval);
	}
    }
    xmlHashFree(hash, xmlHashDefaultDeallocator);
    return(ret);

error:
    xmlHashFree(hash, xmlHashDefaultDeallocator);
    xmlXPathFreeNodeSet(ret);
    return(NULL);
}

/**
 * xmlXPathDistinct:
 * @nodes:  a node-set
 *
 * Implements the EXSLT - Sets distinct() function:
 *    node-set set:distinct (node-set)
 * @nodes is sorted by document order, then #exslSetsDistinctSorted
 * is called with the sorted node-set
 *
 * Returns a subset of the nodes contained in @nodes, or @nodes if
 *         it is empty
 */
xmlNodeSetPtr
xmlXPathDistinct (xmlNodeSetPtr nodes) {
    if (xmlXPathNodeSetIsEmpty(nodes))
	return(nodes);

    xmlXPathNodeSetSort(nodes);
    return(xmlXPathDistinctSorted(nodes));
}

/**
 * xmlXPathHasSameNodes:
 * @nodes1:  a node-set
 * @nodes2:  a node-set
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets has-same-nodes function:
 *    boolean set:has-same-node(node-set, node-set)
 *
 * Returns true (1) if @nodes1 shares any node with @nodes2, false (0)
 *         otherwise
 */
int
xmlXPathHasSameNodes (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    int i, l;
    xmlNodePtr cur;

    if (xmlXPathNodeSetIsEmpty(nodes1) ||
	xmlXPathNodeSetIsEmpty(nodes2))
	return(0);

    l = xmlXPathNodeSetGetLength(nodes1);
    for (i = 0; i < l; i++) {
	cur = xmlXPathNodeSetItem(nodes1, i);
	if (xmlXPathNodeSetContains(nodes2, cur))
	    return(1);
    }
    return(0);
}

/**
 * xmlXPathNodeLeadingSorted:
 * @nodes: a node-set, sorted by document order
 * @node: a node
 *
 * Implements the EXSLT - Sets leading() function:
 *    node-set set:leading (node-set, node-set)
 *
 * DEPRECATED: Don't use.
 *
 * Returns the nodes in @nodes that precede @node in document order,
 *         @nodes if @node is NULL or an empty node-set if @nodes
 *         doesn't contain @node
 */
xmlNodeSetPtr
xmlXPathNodeLeadingSorted (xmlNodeSetPtr nodes, xmlNodePtr node) {
    int i, l;
    xmlNodePtr cur;
    xmlNodeSetPtr ret;

    if (node == NULL)
	return(nodes);

    ret = xmlXPathNodeSetCreate(NULL);
    if (ret == NULL)
        return(ret);
    if (xmlXPathNodeSetIsEmpty(nodes) ||
	(!xmlXPathNodeSetContains(nodes, node)))
	return(ret);

    l = xmlXPathNodeSetGetLength(nodes);
    for (i = 0; i < l; i++) {
	cur = xmlXPathNodeSetItem(nodes, i);
	if (cur == node)
	    break;
	if (xmlXPathNodeSetAddUnique(ret, cur) < 0) {
            xmlXPathFreeNodeSet(ret);
	    return(NULL);
        }
    }
    return(ret);
}

/**
 * xmlXPathNodeLeading:
 * @nodes:  a node-set
 * @node:  a node
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets leading() function:
 *    node-set set:leading (node-set, node-set)
 * @nodes is sorted by document order, then #exslSetsNodeLeadingSorted
 * is called.
 *
 * Returns the nodes in @nodes that precede @node in document order,
 *         @nodes if @node is NULL or an empty node-set if @nodes
 *         doesn't contain @node
 */
xmlNodeSetPtr
xmlXPathNodeLeading (xmlNodeSetPtr nodes, xmlNodePtr node) {
    xmlXPathNodeSetSort(nodes);
    return(xmlXPathNodeLeadingSorted(nodes, node));
}

/**
 * xmlXPathLeadingSorted:
 * @nodes1:  a node-set, sorted by document order
 * @nodes2:  a node-set, sorted by document order
 *
 * Implements the EXSLT - Sets leading() function:
 *    node-set set:leading (node-set, node-set)
 *
 * Returns the nodes in @nodes1 that precede the first node in @nodes2
 *         in document order, @nodes1 if @nodes2 is NULL or empty or
 *         an empty node-set if @nodes1 doesn't contain @nodes2
 */
xmlNodeSetPtr
xmlXPathLeadingSorted (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    if (xmlXPathNodeSetIsEmpty(nodes2))
	return(nodes1);
    return(xmlXPathNodeLeadingSorted(nodes1,
				     xmlXPathNodeSetItem(nodes2, 1)));
}

/**
 * xmlXPathLeading:
 * @nodes1:  a node-set
 * @nodes2:  a node-set
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets leading() function:
 *    node-set set:leading (node-set, node-set)
 * @nodes1 and @nodes2 are sorted by document order, then
 * #exslSetsLeadingSorted is called.
 *
 * Returns the nodes in @nodes1 that precede the first node in @nodes2
 *         in document order, @nodes1 if @nodes2 is NULL or empty or
 *         an empty node-set if @nodes1 doesn't contain @nodes2
 */
xmlNodeSetPtr
xmlXPathLeading (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    if (xmlXPathNodeSetIsEmpty(nodes2))
	return(nodes1);
    if (xmlXPathNodeSetIsEmpty(nodes1))
	return(xmlXPathNodeSetCreate(NULL));
    xmlXPathNodeSetSort(nodes1);
    xmlXPathNodeSetSort(nodes2);
    return(xmlXPathNodeLeadingSorted(nodes1,
				     xmlXPathNodeSetItem(nodes2, 1)));
}

/**
 * xmlXPathNodeTrailingSorted:
 * @nodes: a node-set, sorted by document order
 * @node: a node
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets trailing() function:
 *    node-set set:trailing (node-set, node-set)
 *
 * Returns the nodes in @nodes that follow @node in document order,
 *         @nodes if @node is NULL or an empty node-set if @nodes
 *         doesn't contain @node
 */
xmlNodeSetPtr
xmlXPathNodeTrailingSorted (xmlNodeSetPtr nodes, xmlNodePtr node) {
    int i, l;
    xmlNodePtr cur;
    xmlNodeSetPtr ret;

    if (node == NULL)
	return(nodes);

    ret = xmlXPathNodeSetCreate(NULL);
    if (ret == NULL)
        return(ret);
    if (xmlXPathNodeSetIsEmpty(nodes) ||
	(!xmlXPathNodeSetContains(nodes, node)))
	return(ret);

    l = xmlXPathNodeSetGetLength(nodes);
    for (i = l - 1; i >= 0; i--) {
	cur = xmlXPathNodeSetItem(nodes, i);
	if (cur == node)
	    break;
	if (xmlXPathNodeSetAddUnique(ret, cur) < 0) {
            xmlXPathFreeNodeSet(ret);
	    return(NULL);
        }
    }
    xmlXPathNodeSetSort(ret);	/* bug 413451 */
    return(ret);
}

/**
 * xmlXPathNodeTrailing:
 * @nodes:  a node-set
 * @node:  a node
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets trailing() function:
 *    node-set set:trailing (node-set, node-set)
 * @nodes is sorted by document order, then #xmlXPathNodeTrailingSorted
 * is called.
 *
 * Returns the nodes in @nodes that follow @node in document order,
 *         @nodes if @node is NULL or an empty node-set if @nodes
 *         doesn't contain @node
 */
xmlNodeSetPtr
xmlXPathNodeTrailing (xmlNodeSetPtr nodes, xmlNodePtr node) {
    xmlXPathNodeSetSort(nodes);
    return(xmlXPathNodeTrailingSorted(nodes, node));
}

/**
 * xmlXPathTrailingSorted:
 * @nodes1:  a node-set, sorted by document order
 * @nodes2:  a node-set, sorted by document order
 *
 * Implements the EXSLT - Sets trailing() function:
 *    node-set set:trailing (node-set, node-set)
 *
 * Returns the nodes in @nodes1 that follow the first node in @nodes2
 *         in document order, @nodes1 if @nodes2 is NULL or empty or
 *         an empty node-set if @nodes1 doesn't contain @nodes2
 */
xmlNodeSetPtr
xmlXPathTrailingSorted (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    if (xmlXPathNodeSetIsEmpty(nodes2))
	return(nodes1);
    return(xmlXPathNodeTrailingSorted(nodes1,
				      xmlXPathNodeSetItem(nodes2, 0)));
}

/**
 * xmlXPathTrailing:
 * @nodes1:  a node-set
 * @nodes2:  a node-set
 *
 * DEPRECATED: Don't use.
 *
 * Implements the EXSLT - Sets trailing() function:
 *    node-set set:trailing (node-set, node-set)
 * @nodes1 and @nodes2 are sorted by document order, then
 * #xmlXPathTrailingSorted is called.
 *
 * Returns the nodes in @nodes1 that follow the first node in @nodes2
 *         in document order, @nodes1 if @nodes2 is NULL or empty or
 *         an empty node-set if @nodes1 doesn't contain @nodes2
 */
xmlNodeSetPtr
xmlXPathTrailing (xmlNodeSetPtr nodes1, xmlNodeSetPtr nodes2) {
    if (xmlXPathNodeSetIsEmpty(nodes2))
	return(nodes1);
    if (xmlXPathNodeSetIsEmpty(nodes1))
	return(xmlXPathNodeSetCreate(NULL));
    xmlXPathNodeSetSort(nodes1);
    xmlXPathNodeSetSort(nodes2);
    return(xmlXPathNodeTrailingSorted(nodes1,
				      xmlXPathNodeSetItem(nodes2, 0)));
}

/************************************************************************
 *									*
 *		Routines to handle extra functions			*
 *									*
 ************************************************************************/

/**
 * xmlXPathRegisterFunc:
 * @ctxt:  the XPath context
 * @name:  the function name
 * @f:  the function implementation or NULL
 *
 * Register a new function. If @f is NULL it unregisters the function
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xmlXPathRegisterFunc(xmlXPathContextPtr ctxt, const xmlChar *name,
		     xmlXPathFunction f) {
    return(xmlXPathRegisterFuncNS(ctxt, name, NULL, f));
}

/**
 * xmlXPathRegisterFuncNS:
 * @ctxt:  the XPath context
 * @name:  the function name
 * @ns_uri:  the function namespace URI
 * @f:  the function implementation or NULL
 *
 * Register a new function. If @f is NULL it unregisters the function
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xmlXPathRegisterFuncNS(xmlXPathContextPtr ctxt, const xmlChar *name,
		       const xmlChar *ns_uri, xmlXPathFunction f) {
    int ret;

    if (ctxt == NULL)
	return(-1);
    if (name == NULL)
	return(-1);

    if (ctxt->funcHash == NULL)
	ctxt->funcHash = xmlHashCreate(0);
    if (ctxt->funcHash == NULL) {
        xmlXPathErrMemory(ctxt);
	return(-1);
    }
    if (f == NULL)
        return(xmlHashRemoveEntry2(ctxt->funcHash, name, ns_uri, NULL));
XML_IGNORE_FPTR_CAST_WARNINGS
    ret = xmlHashAddEntry2(ctxt->funcHash, name, ns_uri, (void *) f);
XML_POP_WARNINGS
    if (ret < 0) {
        xmlXPathErrMemory(ctxt);
        return(-1);
    }

    return(0);
}

/**
 * xmlXPathRegisterFuncLookup:
 * @ctxt:  the XPath context
 * @f:  the lookup function
 * @funcCtxt:  the lookup data
 *
 * Registers an external mechanism to do function lookup.
 */
void
xmlXPathRegisterFuncLookup (xmlXPathContextPtr ctxt,
			    xmlXPathFuncLookupFunc f,
			    void *funcCtxt) {
    if (ctxt == NULL)
	return;
    ctxt->funcLookupFunc = f;
    ctxt->funcLookupData = funcCtxt;
}

/**
 * xmlXPathFunctionLookup:
 * @ctxt:  the XPath context
 * @name:  the function name
 *
 * Search in the Function array of the context for the given
 * function.
 *
 * Returns the xmlXPathFunction or NULL if not found
 */
xmlXPathFunction
xmlXPathFunctionLookup(xmlXPathContextPtr ctxt, const xmlChar *name) {
    return(xmlXPathFunctionLookupNS(ctxt, name, NULL));
}

static const xmlXPathStandardFunction *
xmlXPathLookupStandardFunction(const xmlChar *name) {
    int bucketIndex = xmlXPathSFComputeHash(name) % SF_HASH_SIZE;

    while (xmlXPathSFHash[bucketIndex] != UCHAR_MAX) {
        int funcIndex = xmlXPathSFHash[bucketIndex];

        if (strcmp(xmlXPathStandardFunctions[funcIndex].name,
                   (char *) name) == 0)
            return(&xmlXPathStandardFunctions[funcIndex]);

        bucketIndex += 1;
        if (bucketIndex >= SF_HASH_SIZE)
            bucketIndex = 0;
    }

    return(NULL);
}

/**
 * xmlXPathFunctionLookupNS:
 * @ctxt:  the XPath context
 * @name:  the function name
 * @ns_uri:  the function namespace URI
 *
 * Search in the Function array of the context for the given
 * function.
 *
 * Returns the xmlXPathFunction or NULL if not found
 */
xmlXPathFunction
xmlXPathFunctionLookupNS(xmlXPathContextPtr ctxt, const xmlChar *name,
			 const xmlChar *ns_uri) {
    xmlXPathFunction ret;

    if (ctxt == NULL)
	return(NULL);
    if (name == NULL)
	return(NULL);

    if (ctxt->funcLookupFunc != NULL) {
	xmlXPathFuncLookupFunc f;

	f = ctxt->funcLookupFunc;
	ret = f(ctxt->funcLookupData, name, ns_uri);
	if (ret != NULL)
	    return(ret);
    }

    if (ctxt->funcHash == NULL)
	return(NULL);

XML_IGNORE_FPTR_CAST_WARNINGS
    ret = (xmlXPathFunction) xmlHashLookup2(ctxt->funcHash, name, ns_uri);
XML_POP_WARNINGS
    return(ret);
}

/**
 * xmlXPathRegisteredFuncsCleanup:
 * @ctxt:  the XPath context
 *
 * Cleanup the XPath context data associated to registered functions
 */
void
xmlXPathRegisteredFuncsCleanup(xmlXPathContextPtr ctxt) {
    if (ctxt == NULL)
	return;

    xmlHashFree(ctxt->funcHash, NULL);
    ctxt->funcHash = NULL;
}

/************************************************************************
 *									*
 *			Routines to handle Variables			*
 *									*
 ************************************************************************/

/**
 * xmlXPathRegisterVariable:
 * @ctxt:  the XPath context
 * @name:  the variable name
 * @value:  the variable value or NULL
 *
 * Register a new variable value. If @value is NULL it unregisters
 * the variable
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xmlXPathRegisterVariable(xmlXPathContextPtr ctxt, const xmlChar *name,
			 xmlXPathObjectPtr value) {
    return(xmlXPathRegisterVariableNS(ctxt, name, NULL, value));
}

/**
 * xmlXPathRegisterVariableNS:
 * @ctxt:  the XPath context
 * @name:  the variable name
 * @ns_uri:  the variable namespace URI
 * @value:  the variable value or NULL
 *
 * Register a new variable value. If @value is NULL it unregisters
 * the variable
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xmlXPathRegisterVariableNS(xmlXPathContextPtr ctxt, const xmlChar *name,
			   const xmlChar *ns_uri,
			   xmlXPathObjectPtr value) {
    if (ctxt == NULL)
	return(-1);
    if (name == NULL)
	return(-1);

    if (ctxt->varHash == NULL)
	ctxt->varHash = xmlHashCreate(0);
    if (ctxt->varHash == NULL)
	return(-1);
    if (value == NULL)
        return(xmlHashRemoveEntry2(ctxt->varHash, name, ns_uri,
	                           xmlXPathFreeObjectEntry));
    return(xmlHashUpdateEntry2(ctxt->varHash, name, ns_uri,
			       (void *) value, xmlXPathFreeObjectEntry));
}

/**
 * xmlXPathRegisterVariableLookup:
 * @ctxt:  the XPath context
 * @f:  the lookup function
 * @data:  the lookup data
 *
 * register an external mechanism to do variable lookup
 */
void
xmlXPathRegisterVariableLookup(xmlXPathContextPtr ctxt,
	 xmlXPathVariableLookupFunc f, void *data) {
    if (ctxt == NULL)
	return;
    ctxt->varLookupFunc = f;
    ctxt->varLookupData = data;
}

/**
 * xmlXPathVariableLookup:
 * @ctxt:  the XPath context
 * @name:  the variable name
 *
 * Search in the Variable array of the context for the given
 * variable value.
 *
 * Returns a copy of the value or NULL if not found
 */
xmlXPathObjectPtr
xmlXPathVariableLookup(xmlXPathContextPtr ctxt, const xmlChar *name) {
    return(xmlXPathVariableLookupNS(ctxt, name, NULL));
}

/**
 * xmlXPathVariableLookupNS:
 * @ctxt:  the XPath context
 * @name:  the variable name
 * @ns_uri:  the variable namespace URI
 *
 * Search in the Variable array of the context for the given
 * variable value.
 *
 * Returns the a copy of the value or NULL if not found
 */
xmlXPathObjectPtr
xmlXPathVariableLookupNS(xmlXPathContextPtr ctxt, const xmlChar *name,
			 const xmlChar *ns_uri) {
    if (ctxt == NULL)
	return(NULL);

    if (ctxt->varLookupFunc != NULL) {
	xmlXPathObjectPtr ret;

	ret = ctxt->varLookupFunc(ctxt->varLookupData, name, ns_uri);
	if (ret != NULL) return(ret);
    }

    if (ctxt->varHash == NULL)
	return(NULL);
    if (name == NULL)
	return(NULL);

    return(xmlXPathObjectCopy(xmlHashLookup2(ctxt->varHash, name, ns_uri)));
}

/**
 * xmlXPathRegisteredVariablesCleanup:
 * @ctxt:  the XPath context
 *
 * Cleanup the XPath context data associated to registered variables
 */
void
xmlXPathRegisteredVariablesCleanup(xmlXPathContextPtr ctxt) {
    if (ctxt == NULL)
	return;

    xmlHashFree(ctxt->varHash, xmlXPathFreeObjectEntry);
    ctxt->varHash = NULL;
}

/**
 * xmlXPathRegisterNs:
 * @ctxt:  the XPath context
 * @prefix:  the namespace prefix cannot be NULL or empty string
 * @ns_uri:  the namespace name
 *
 * Register a new namespace. If @ns_uri is NULL it unregisters
 * the namespace
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xmlXPathRegisterNs(xmlXPathContextPtr ctxt, const xmlChar *prefix,
			   const xmlChar *ns_uri) {
    xmlChar *copy;

    if (ctxt == NULL)
	return(-1);
    if (prefix == NULL)
	return(-1);
    if (prefix[0] == 0)
	return(-1);

    if (ctxt->nsHash == NULL)
	ctxt->nsHash = xmlHashCreate(10);
    if (ctxt->nsHash == NULL) {
        xmlXPathErrMemory(ctxt);
	return(-1);
    }
    if (ns_uri == NULL)
        return(xmlHashRemoveEntry(ctxt->nsHash, prefix,
	                          xmlHashDefaultDeallocator));

    copy = xmlStrdup(ns_uri);
    if (copy == NULL) {
        xmlXPathErrMemory(ctxt);
        return(-1);
    }
    if (xmlHashUpdateEntry(ctxt->nsHash, prefix, copy,
                           xmlHashDefaultDeallocator) < 0) {
        xmlXPathErrMemory(ctxt);
        xmlFree(copy);
        return(-1);
    }

    return(0);
}

/**
 * xmlXPathNsLookup:
 * @ctxt:  the XPath context
 * @prefix:  the namespace prefix value
 *
 * Search in the namespace declaration array of the context for the given
 * namespace name associated to the given prefix
 *
 * Returns the value or NULL if not found
 */
const xmlChar *
xmlXPathNsLookup(xmlXPathContextPtr ctxt, const xmlChar *prefix) {
    if (ctxt == NULL)
	return(NULL);
    if (prefix == NULL)
	return(NULL);

    if (xmlStrEqual(prefix, (const xmlChar *) "xml"))
	return(XML_XML_NAMESPACE);

    if (ctxt->namespaces != NULL) {
	int i;

	for (i = 0;i < ctxt->nsNr;i++) {
	    if ((ctxt->namespaces[i] != NULL) &&
		(xmlStrEqual(ctxt->namespaces[i]->prefix, prefix)))
		return(ctxt->namespaces[i]->href);
	}
    }

    return((const xmlChar *) xmlHashLookup(ctxt->nsHash, prefix));
}

/**
 * xmlXPathRegisteredNsCleanup:
 * @ctxt:  the XPath context
 *
 * Cleanup the XPath context data associated to registered variables
 */
void
xmlXPathRegisteredNsCleanup(xmlXPathContextPtr ctxt) {
    if (ctxt == NULL)
	return;

    xmlHashFree(ctxt->nsHash, xmlHashDefaultDeallocator);
    ctxt->nsHash = NULL;
}

/************************************************************************
 *									*
 *			Routines to handle Values			*
 *									*
 ************************************************************************/

/* Allocations are terrible, one needs to optimize all this !!! */

/**
 * xmlXPathNewFloat:
 * @val:  the double value
 *
 * Create a new xmlXPathObjectPtr of type double and of value @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewFloat(double val) {
    xmlXPathObjectPtr ret;

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_NUMBER;
    ret->floatval = val;
    return(ret);
}

/**
 * xmlXPathNewBoolean:
 * @val:  the boolean value
 *
 * Create a new xmlXPathObjectPtr of type boolean and of value @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewBoolean(int val) {
    xmlXPathObjectPtr ret;

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_BOOLEAN;
    ret->boolval = (val != 0);
    return(ret);
}

/**
 * xmlXPathNewString:
 * @val:  the xmlChar * value
 *
 * Create a new xmlXPathObjectPtr of type string and of value @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewString(const xmlChar *val) {
    xmlXPathObjectPtr ret;

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_STRING;
    if (val == NULL)
        val = BAD_CAST "";
    ret->stringval = xmlStrdup(val);
    if (ret->stringval == NULL) {
        xmlFree(ret);
        return(NULL);
    }
    return(ret);
}

/**
 * xmlXPathWrapString:
 * @val:  the xmlChar * value
 *
 * Wraps the @val string into an XPath object.
 *
 * Returns the newly created object.
 *
 * Frees @val in case of error.
 */
xmlXPathObjectPtr
xmlXPathWrapString (xmlChar *val) {
    xmlXPathObjectPtr ret;

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL) {
        xmlFree(val);
	return(NULL);
    }
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_STRING;
    ret->stringval = val;
    return(ret);
}

/**
 * xmlXPathNewCString:
 * @val:  the char * value
 *
 * Create a new xmlXPathObjectPtr of type string and of value @val
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathNewCString(const char *val) {
    return(xmlXPathNewString(BAD_CAST val));
}

/**
 * xmlXPathWrapCString:
 * @val:  the char * value
 *
 * Wraps a string into an XPath object.
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathWrapCString (char * val) {
    return(xmlXPathWrapString((xmlChar *)(val)));
}

/**
 * xmlXPathWrapExternal:
 * @val:  the user data
 *
 * Wraps the @val data into an XPath object.
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathWrapExternal (void *val) {
    xmlXPathObjectPtr ret;

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathObject));
    ret->type = XPATH_USERS;
    ret->user = val;
    return(ret);
}

/**
 * xmlXPathObjectCopy:
 * @val:  the original object
 *
 * allocate a new copy of a given object
 *
 * Returns the newly created object.
 */
xmlXPathObjectPtr
xmlXPathObjectCopy(xmlXPathObjectPtr val) {
    xmlXPathObjectPtr ret;

    if (val == NULL)
	return(NULL);

    ret = (xmlXPathObjectPtr) xmlMalloc(sizeof(xmlXPathObject));
    if (ret == NULL)
	return(NULL);
    memcpy(ret, val , sizeof(xmlXPathObject));
    switch (val->type) {
	case XPATH_BOOLEAN:
	case XPATH_NUMBER:
	    break;
	case XPATH_STRING:
	    ret->stringval = xmlStrdup(val->stringval);
            if (ret->stringval == NULL) {
                xmlFree(ret);
                return(NULL);
            }
	    break;
	case XPATH_XSLT_TREE:
	case XPATH_NODESET:
	    ret->nodesetval = xmlXPathNodeSetCopy(val->nodesetval);
            if (ret->nodesetval == NULL) {
                xmlFree(ret);
                return(NULL);
            }
	    /* Do not deallocate the copied tree value */
	    ret->boolval = 0;
	    break;
        case XPATH_USERS:
	    ret->user = val->user;
	    break;
        default:
            xmlFree(ret);
            ret = NULL;
	    break;
    }
    return(ret);
}

/**
 * xmlXPathFreeObject:
 * @obj:  the object to free
 *
 * Free up an xmlXPathObjectPtr object.
 */
void
xmlXPathFreeObject(xmlXPathObjectPtr obj) {
    if (obj == NULL) return;
    if ((obj->type == XPATH_NODESET) || (obj->type == XPATH_XSLT_TREE)) {
        if (obj->nodesetval != NULL)
            xmlXPathFreeNodeSet(obj->nodesetval);
    } else if (obj->type == XPATH_STRING) {
	if (obj->stringval != NULL)
	    xmlFree(obj->stringval);
    }
    xmlFree(obj);
}

static void
xmlXPathFreeObjectEntry(void *obj, const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlXPathFreeObject((xmlXPathObjectPtr) obj);
}

/**
 * xmlXPathReleaseObject:
 * @obj:  the xmlXPathObjectPtr to free or to cache
 *
 * Depending on the state of the cache this frees the given
 * XPath object or stores it in the cache.
 */
static void
xmlXPathReleaseObject(xmlXPathContextPtr ctxt, xmlXPathObjectPtr obj)
{
    xmlXPathContextCachePtr cache;

    if (obj == NULL)
        return;

    cache = ctxt->cache;
    if (cache == NULL) {
        xmlXPathFreeObject(obj);
        return;
    }

    switch (obj->type) {
        case XPATH_NODESET:
        case XPATH_XSLT_TREE: {
            xmlNodeSetPtr set = obj->nodesetval;

            if (set != NULL) {
                xmlXPathNodeSetClear(set, 1);

                if ((set->nodeMax == XML_NODESET_DEFAULT) &&
                    (cache->numNodeArrays < cache->maxNodeArrays)) {
                    set->nodeTab[0] = (void *) cache->nodeArrays;
                    cache->nodeArrays = set->nodeTab;
                    cache->numNodeArrays += 1;
                } else {
                    xmlFree(set->nodeTab);
                }

                if (cache->numNodeset < cache->maxNodeset) {
                    obj->stringval = (void *) cache->nodesetObjs;
                    cache->nodesetObjs = obj;
                    cache->numNodeset += 1;

                    return;
                }

                xmlFree(set);
            }
            break;
        }

        case XPATH_STRING:
            if (obj->stringval != NULL)
                xmlFree(obj->stringval);
            obj->stringval = NULL;
            break;

        default:
            break;
    }

    /*
    * Fallback to adding to the misc-objects slot.
    */
    if (cache->numMisc < cache->maxMisc) {
        obj->stringval = (void *) cache->miscObjs;
        cache->miscObjs = obj;
        cache->numMisc += 1;

        return;
    }

    /*
    * Cache is full; free the object.
    */
    xmlFree(obj);
}


/************************************************************************
 *									*
 *			Type Casting Routines				*
 *									*
 ************************************************************************/

/**
 * xmlXPathCastBooleanToString:
 * @val:  a boolean
 *
 * Converts a boolean to its string value.
 *
 * Returns a newly allocated string.
 */
xmlChar *
xmlXPathCastBooleanToString (int val) {
    xmlChar *ret;
    if (val)
	ret = xmlStrdup((const xmlChar *) "true");
    else
	ret = xmlStrdup((const xmlChar *) "false");
    return(ret);
}

/**
 * xmlXPathCastNumberToString:
 * @val:  a number
 *
 * Converts a number to its string value.
 *
 * Returns a newly allocated string.
 */
xmlChar *
xmlXPathCastNumberToString (double val) {
    xmlChar *ret;
    switch (xmlXPathIsInf(val)) {
    case 1:
	ret = xmlStrdup((const xmlChar *) "Infinity");
	break;
    case -1:
	ret = xmlStrdup((const xmlChar *) "-Infinity");
	break;
    default:
	if (xmlXPathIsNaN(val)) {
	    ret = xmlStrdup((const xmlChar *) "NaN");
	} else if (val == 0) {
            /* Omit sign for negative zero. */
	    ret = xmlStrdup((const xmlChar *) "0");
	} else {
	    /* could be improved */
	    char buf[100];
	    xmlXPathFormatNumber(val, buf, 99);
	    buf[99] = 0;
	    ret = xmlStrdup((const xmlChar *) buf);
	}
    }
    return(ret);
}

/**
 * xmlXPathCastNodeToString:
 * @node:  a node
 *
 * Converts a node to its string value.
 *
 * Returns a newly allocated string.
 */
xmlChar *
xmlXPathCastNodeToString (xmlNodePtr node) {
    return(xmlNodeGetContent(node));
}

/**
 * xmlXPathCastNodeSetToString:
 * @ns:  a node-set
 *
 * Converts a node-set to its string value.
 *
 * Returns a newly allocated string.
 */
xmlChar *
xmlXPathCastNodeSetToString (xmlNodeSetPtr ns) {
    if ((ns == NULL) || (ns->nodeNr == 0) || (ns->nodeTab == NULL))
	return(xmlStrdup((const xmlChar *) ""));

    return(xmlXPathCastNodeToString(ns->nodeTab[0]));
}

/**
 * xmlXPathCastToString:
 * @val:  an XPath object
 *
 * Converts an existing object to its string() equivalent
 *
 * Returns the allocated string value of the object, NULL in case of error.
 *         It's up to the caller to free the string memory with xmlFree().
 */
xmlChar *
xmlXPathCastToString(xmlXPathObjectPtr val) {
    xmlChar *ret = NULL;

    if (val == NULL)
	return(xmlStrdup((const xmlChar *) ""));
    switch (val->type) {
	case XPATH_UNDEFINED:
	    ret = xmlStrdup((const xmlChar *) "");
	    break;
        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
	    ret = xmlXPathCastNodeSetToString(val->nodesetval);
	    break;
	case XPATH_STRING:
	    return(xmlStrdup(val->stringval));
        case XPATH_BOOLEAN:
	    ret = xmlXPathCastBooleanToString(val->boolval);
	    break;
	case XPATH_NUMBER: {
	    ret = xmlXPathCastNumberToString(val->floatval);
	    break;
	}
	case XPATH_USERS:
	    /* TODO */
	    ret = xmlStrdup((const xmlChar *) "");
	    break;
    }
    return(ret);
}

/**
 * xmlXPathConvertString:
 * @val:  an XPath object
 *
 * Converts an existing object to its string() equivalent
 *
 * Returns the new object, the old one is freed (or the operation
 *         is done directly on @val)
 */
xmlXPathObjectPtr
xmlXPathConvertString(xmlXPathObjectPtr val) {
    xmlChar *res;

    if ((val != NULL) && (val->type == XPATH_STRING))
	return(val);

    res = xmlXPathCastToString(val);
    xmlXPathFreeObject(val);
    if (res == NULL)
        return(NULL);

    return(xmlXPathWrapString(res));
}

/**
 * xmlXPathCastBooleanToNumber:
 * @val:  a boolean
 *
 * Converts a boolean to its number value
 *
 * Returns the number value
 */
double
xmlXPathCastBooleanToNumber(int val) {
    if (val)
	return(1.0);
    return(0.0);
}

/**
 * xmlXPathCastStringToNumber:
 * @val:  a string
 *
 * Converts a string to its number value
 *
 * Returns the number value
 */
double
xmlXPathCastStringToNumber(const xmlChar * val) {
    return(xmlXPathStringEvalNumber(val));
}

/**
 * xmlXPathNodeToNumberInternal:
 * @node:  a node
 *
 * Converts a node to its number value
 *
 * Returns the number value
 */
static double
xmlXPathNodeToNumberInternal(xmlXPathContextPtr ctxt, xmlNodePtr node) {
    xmlChar *strval;
    double ret;

    if (node == NULL)
	return(xmlXPathNAN);
    strval = xmlXPathCastNodeToString(node);
    if (strval == NULL) {
        xmlXPathErrMemory(ctxt);
	return(xmlXPathNAN);
    }
    ret = xmlXPathCastStringToNumber(strval);
    xmlFree(strval);

    return(ret);
}

/**
 * xmlXPathCastNodeToNumber:
 * @node:  a node
 *
 * Converts a node to its number value
 *
 * Returns the number value
 */
double
xmlXPathCastNodeToNumber (xmlNodePtr node) {
    return(xmlXPathNodeToNumberInternal(NULL, node));
}

/**
 * xmlXPathCastNodeSetToNumber:
 * @ns:  a node-set
 *
 * Converts a node-set to its number value
 *
 * Returns the number value
 */
double
xmlXPathCastNodeSetToNumber (xmlNodeSetPtr ns) {
    xmlChar *str;
    double ret;

    if (ns == NULL)
	return(xmlXPathNAN);
    str = xmlXPathCastNodeSetToString(ns);
    ret = xmlXPathCastStringToNumber(str);
    xmlFree(str);
    return(ret);
}

/**
 * xmlXPathCastToNumber:
 * @val:  an XPath object
 *
 * Converts an XPath object to its number value
 *
 * Returns the number value
 */
double
xmlXPathCastToNumber(xmlXPathObjectPtr val) {
    return(xmlXPathCastToNumberInternal(NULL, val));
}

/**
 * xmlXPathConvertNumber:
 * @val:  an XPath object
 *
 * Converts an existing object to its number() equivalent
 *
 * Returns the new object, the old one is freed (or the operation
 *         is done directly on @val)
 */
xmlXPathObjectPtr
xmlXPathConvertNumber(xmlXPathObjectPtr val) {
    xmlXPathObjectPtr ret;

    if ((val != NULL) && (val->type == XPATH_NUMBER))
	return(val);

    ret = xmlXPathNewFloat(xmlXPathCastToNumber(val));
    xmlXPathFreeObject(val);
    return(ret);
}

/**
 * xmlXPathCastNumberToBoolean:
 * @val:  a number
 *
 * Converts a number to its boolean value
 *
 * Returns the boolean value
 */
int
xmlXPathCastNumberToBoolean (double val) {
     if (xmlXPathIsNaN(val) || (val == 0.0))
	 return(0);
     return(1);
}

/**
 * xmlXPathCastStringToBoolean:
 * @val:  a string
 *
 * Converts a string to its boolean value
 *
 * Returns the boolean value
 */
int
xmlXPathCastStringToBoolean (const xmlChar *val) {
    if ((val == NULL) || (val[0] == 0))
	return(0);
    return(1);
}

/**
 * xmlXPathCastNodeSetToBoolean:
 * @ns:  a node-set
 *
 * Converts a node-set to its boolean value
 *
 * Returns the boolean value
 */
int
xmlXPathCastNodeSetToBoolean (xmlNodeSetPtr ns) {
    if ((ns == NULL) || (ns->nodeNr == 0))
	return(0);
    return(1);
}

/**
 * xmlXPathCastToBoolean:
 * @val:  an XPath object
 *
 * Converts an XPath object to its boolean value
 *
 * Returns the boolean value
 */
int
xmlXPathCastToBoolean(xmlXPathObjectPtr val) {
    int ret = 0;

    if (val == NULL)
	return(0);

    switch (val->type) {
    case XPATH_NODESET:
    case XPATH_XSLT_TREE:
	ret = ((val->nodesetval != NULL) && (val->nodesetval->nodeNr > 0));
	break;
    case XPATH_STRING:
	ret = ((val->stringval != NULL) && (val->stringval[0] != 0));
	break;
    case XPATH_NUMBER:
	ret = (val->floatval != 0.0);;
	break;
    case XPATH_BOOLEAN:
	ret = val->boolval;
	break;
    default:
	break;
    }

    return(ret);
}


/**
 * xmlXPathConvertBoolean:
 * @val:  an XPath object
 *
 * Converts an existing object to its boolean() equivalent
 *
 * Returns the new object, the old one is freed (or the operation
 *         is done directly on @val)
 */
xmlXPathObjectPtr
xmlXPathConvertBoolean(xmlXPathObjectPtr val) {
    xmlXPathObjectPtr ret;

    if ((val != NULL) && (val->type == XPATH_BOOLEAN))
	return(val);

    ret = xmlXPathNewBoolean(xmlXPathCastToBoolean(val));
    xmlXPathFreeObject(val);
    return(ret);
}

/************************************************************************
 *									*
 *		Routines to handle XPath contexts			*
 *									*
 ************************************************************************/

/**
 * xmlXPathNewContext:
 * @doc:  the XML document
 *
 * Create a new xmlXPathContext
 *
 * Returns the xmlXPathContext just allocated. The caller will need to free it.
 */
xmlXPathContextPtr
xmlXPathNewContext(xmlDocPtr doc) {
    xmlXPathContextPtr ret;

    ret = (xmlXPathContextPtr) xmlMalloc(sizeof(xmlXPathContext));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0 , sizeof(xmlXPathContext));
    ret->doc = doc;
    ret->node = NULL;

    ret->varHash = NULL;

    ret->nb_types = 0;
    ret->max_types = 0;
    ret->types = NULL;

    ret->nb_axis = 0;
    ret->max_axis = 0;
    ret->axis = NULL;

    ret->nsHash = NULL;
    ret->user = NULL;

    ret->contextSize = 1;
    ret->proximityPosition = 1;

#ifdef XP_DEFAULT_CACHE_ON
    if (xmlXPathContextSetCache(ret, 1, -1, 0) == -1) {
	xmlXPathFreeContext(ret);
	return(NULL);
    }
#endif

    ret->pctxt.context = ret;

    return(ret);
}

/**
 * xmlXPathFreeContext:
 * @ctxt:  the context to free
 *
 * Free up an xmlXPathContext
 */
void
xmlXPathFreeContext(xmlXPathContextPtr ctxt) {
    if (ctxt == NULL) return;

    if (ctxt->cache != NULL)
	xmlXPathFreeCache((xmlXPathContextCachePtr) ctxt->cache);
    xmlXPathRegisteredNsCleanup(ctxt);
    xmlXPathRegisteredFuncsCleanup(ctxt);
    xmlXPathRegisteredVariablesCleanup(ctxt);
    xmlResetError(&ctxt->lastError);
    xmlFree(ctxt->pctxt.valueTab);
    xmlFree(ctxt);
}

/**
 * xmlXPathSetErrorHandler:
 * @ctxt:  the XPath context
 * @handler:  error handler
 * @data:  user data which will be passed to the handler
 *
 * Register a callback function that will be called on errors and
 * warnings. If handler is NULL, the error handler will be deactivated.
 *
 * Available since 2.13.0.
 */
void
xmlXPathSetErrorHandler(xmlXPathContextPtr ctxt,
                        xmlStructuredErrorFunc handler, void *data) {
    if (ctxt == NULL)
        return;

    ctxt->serror = handler;
    ctxt->userData = data;
}

/************************************************************************
 *									*
 *		Routines to handle XPath parser contexts		*
 *									*
 ************************************************************************/

/**
 * xmlXPathNewParserContext:
 * @str:  the XPath expression
 * @ctxt:  the XPath context
 *
 * Create a new xmlXPathParserContext
 *
 * Returns the xmlXPathParserContext just allocated.
 */
xmlXPathParserContextPtr
xmlXPathNewParserContext(const xmlChar *str, xmlXPathContextPtr ctxt) {
    xmlXPathParserContextPtr ret;

    xmlInitParser();

    ret = (xmlXPathParserContextPtr) xmlMalloc(sizeof(xmlXPathParserContext));
    if (ret == NULL) {
        xmlXPathErrMemory(ctxt);
	return(NULL);
    }
    memset(ret, 0 , sizeof(xmlXPathParserContext));
    ret->cur = ret->base = str;
    ret->context = ctxt;

    return(ret);
}

/**
 * xmlXPathFreeParserContext:
 * @ctxt:  the context to free
 *
 * Free up an xmlXPathParserContext
 */
void
xmlXPathFreeParserContext(xmlXPathParserContextPtr ctxt) {
    int i;

    if (ctxt->valueTab != NULL) {
        for (i = 0; i < ctxt->valueNr; i++) {
            if (ctxt->context)
                xmlXPathReleaseObject(ctxt->context, ctxt->valueTab[i]);
            else
                xmlXPathFreeObject(ctxt->valueTab[i]);
        }
        xmlFree(ctxt->valueTab);
    }
    if (ctxt->comp != NULL) {
#ifdef XPATH_STREAMING
	if (ctxt->comp->stream != NULL) {
	    xmlFreePatternList(ctxt->comp->stream);
	    ctxt->comp->stream = NULL;
	}
#endif
	xmlXPathFreeCompExpr(ctxt->comp);
    }
    xmlFree(ctxt);
}

/************************************************************************
 *									*
 *		The implicit core function library			*
 *									*
 ************************************************************************/

static void
xmlXPathBooleanFuncInternal(xmlXPathParserContextPtr ctxt) {
    xmlXPathObjectPtr obj;
    int boolval;

    boolval = xmlXPathPopBoolean(ctxt);

    obj = xmlXPathCacheNewBoolean(ctxt->context, boolval);
    if (obj != NULL)
        valuePush(ctxt, obj);
}

static void
xmlXPathNumberFuncInternal(xmlXPathParserContextPtr ctxt) {
    xmlXPathObjectPtr obj;
    double floatval;

    floatval = xmlXPathPopNumber(ctxt);

    obj = xmlXPathCacheNewFloat(ctxt->context, floatval);
    if (obj != NULL)
        valuePush(ctxt, obj);
}

static void
xmlXPathItemReleaseString(xmlXPathItem *item) {
    if (!item->isCopy)
        xmlFree(item->as.string);
}

static int
xmlXPathItemInitNodeSet(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    xmlXPathContextCachePtr cache = ctxt->cache;
    xmlNodePtr *nodes;

    if ((cache != NULL) && (cache->nodeArrays != NULL)) {
        nodes = cache->nodeArrays;
        cache->nodeArrays = (void *) nodes[0];
        cache->numNodeArrays -= 1;
    } else {
        nodes = xmlMalloc(XML_NODESET_DEFAULT * sizeof(nodes[0]));
        if (nodes == NULL) {
            xmlXPathErrMemory(ctxt);
            return(-1);
        }
    }

    item->type = XPATH_NODESET;
    item->isCopy = 0;
    item->hasNsNodes = 0;
    item->as.nodeset.nodeTab = nodes;
    item->as.nodeset.nodeMax = XML_NODESET_DEFAULT;
    item->as.nodeset.nodeNr = 0;

    return(0);
}

static int
xmlXPathItemCloneNodeSet(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    xmlXPathItem orig;
    int res;

    orig = *item;

    item->isCopy = 0;
    item->hasNsNodes = 0;
    item->as.nodeset.nodeTab = NULL;
    item->as.nodeset.nodeMax = 0;
    item->as.nodeset.nodeNr = 0;

    res = xmlXPathCacheNodeSetCopy(ctxt, &item->as.nodeset, &orig.as.nodeset);
    if (res < 0)
        return(-1);

    if (res > 0)
        item->hasNsNodes = 1;

    return(0);
}

static void
xmlXPathItemReleaseNodeSet(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    xmlXPathContextCachePtr cache;

    if (item->isCopy)
        return;

    if (item->hasNsNodes)
        xmlXPathNodeSetClear(&item->as.nodeset, 1);

    cache = ctxt->cache;
    if ((cache != NULL) &&
        (cache->numNodeArrays < cache->maxNodeArrays) &&
        (item->as.nodeset.nodeMax == XML_NODESET_DEFAULT)) {
        item->as.nodeset.nodeTab[0] = (void *) cache->nodeArrays;
        cache->nodeArrays = item->as.nodeset.nodeTab;
        cache->numNodeArrays += 1;
    } else {
        xmlFree(item->as.nodeset.nodeTab);
    }
}

static void
xmlXPathItemRelease(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    switch (item->type) {
        case XPATH_STRING:
            xmlXPathItemReleaseString(item);
            break;

        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
            xmlXPathItemReleaseNodeSet(ctxt, item);
            break;

        default:
            break;
    }
}

static void
xmlXPathItemFromObj(xmlXPathContextPtr ctxt, xmlXPathItem *item,
                    xmlXPathObjectPtr obj, int copy) {
    item->type = obj->type;

    switch (obj->type) {
        case XPATH_BOOLEAN:
            item->as.boolean = obj->boolval;
            break;

        case XPATH_NUMBER:
            item->as.number = obj->floatval;
            break;

        case XPATH_STRING:
            item->as.string = obj->stringval;
            item->isCopy = copy;

            if (!copy)
                obj->stringval = NULL;
            break;

        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
            item->as.nodeset = *obj->nodesetval;
            item->isCopy = copy;
            item->hasNsNodes = 1;

            if (!copy) {
                obj->nodesetval->nodeTab = NULL;
                obj->nodesetval->nodeMax = 0;
                obj->nodesetval->nodeNr = 0;
            }
            break;

        case XPATH_USERS:
            item->as.user = obj->user;
            break;

        default:
            xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
            break;
    }

    if (!copy)
        xmlXPathReleaseObject(ctxt, obj);
}

static xmlXPathObjectPtr
xmlXPathItemToObj(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    xmlXPathObjectPtr obj;

    switch (item->type) {
        case XPATH_BOOLEAN:
            obj = xmlXPathCacheNewBoolean(ctxt, item->as.boolean);
            break;

        case XPATH_NUMBER:
            obj = xmlXPathCacheNewFloat(ctxt, item->as.number);
            break;

        case XPATH_STRING:
            if (item->isCopy)
                obj = xmlXPathCacheNewString(ctxt, item->as.string);
            else
                obj = xmlXPathCacheWrapString(ctxt, item->as.string);
            break;

        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
            obj = xmlXPathCacheNewNodeSet(ctxt);
            if (obj == NULL) {
                xmlXPathItemReleaseNodeSet(ctxt, item);
                break;
            }

            obj->type = item->type;

            if (item->isCopy) {
                if (xmlXPathCacheNodeSetCopy(ctxt, obj->nodesetval,
                                             &item->as.nodeset) < 0) {
                    xmlXPathFreeObject(obj);
                    obj = NULL;
                }
            } else {
                *obj->nodesetval = item->as.nodeset;
            }
            break;

        case XPATH_USERS:
            obj = xmlXPathCacheNewExternal(ctxt, item->as.user);
            break;

        default:
            xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
            return(NULL);
    }

    if (obj == NULL)
        xmlXPathErrMemory(ctxt);

    return(obj);
}

static int
xmlXPathItemToBoolean(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    int ret;

    switch (item->type) {
        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
            ret = (item->as.nodeset.nodeNr > 0);
            xmlXPathItemReleaseNodeSet(ctxt, item);
            break;
        case XPATH_STRING:
            ret = (item->as.string[0] != 0);
            xmlXPathItemReleaseString(item);
            break;
        case XPATH_NUMBER:
            ret = (item->as.number != 0.0);
            break;
        case XPATH_BOOLEAN:
            ret = item->as.boolean;
            break;
        default:
            xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
            ret = -1;
            break;
    }

    return(ret);
}

static double
xmlXPathItemToNumber(xmlXPathContextPtr ctxt, xmlXPathItem *item) {
    double ret = xmlXPathNAN;

    switch (item->type) {
        case XPATH_NODESET:
        case XPATH_XSLT_TREE: {
            xmlChar *str;

            if (item->as.nodeset.nodeNr > 0) {
                str = xmlNodeGetContent(item->as.nodeset.nodeTab[0]);
                if (str == NULL) {
                    xmlXPathErrMemory(ctxt);
                } else {
                    ret = xmlXPathCastStringToNumber(str);
                    xmlFree(str);
                }
            }

            xmlXPathItemReleaseNodeSet(ctxt, item);
            break;
        }

        case XPATH_STRING:
            ret = xmlXPathCastStringToNumber(item->as.string);
            xmlXPathItemReleaseString(item);
            break;

        case XPATH_NUMBER:
            ret = item->as.number;
            break;

        case XPATH_BOOLEAN:
            ret = (item->as.boolean) ? 1.0 : 0.0;
            break;

        default:
            xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
            break;
    }

    return(ret);
}

static int
xmlXPathItemToString(xmlXPathContextPtr ctxt, xmlXPathItem *result,
                     xmlXPathItem *item) {
    xmlChar *string;
    int isCopy;

    switch (item->type) {
        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
            if (item->as.nodeset.nodeNr <= 0) {
                string = BAD_CAST "";
                isCopy = 1;
            } else {
                string = xmlNodeGetContent(item->as.nodeset.nodeTab[0]);
                isCopy = 0;
            }
            xmlXPathItemReleaseNodeSet(ctxt, item);
	    break;

	case XPATH_STRING:
            string = item->as.string;
            isCopy = item->isCopy;;
            break;

        case XPATH_BOOLEAN:
	    string = BAD_CAST ((item->as.boolean) ? "true" : "false");
            isCopy = 1;
	    break;

	case XPATH_NUMBER:
	    string = xmlXPathCastNumberToString(item->as.number);
            isCopy = 0;
	    break;

        default:
            xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
            return(-1);
    }

    if (string == NULL) {
        xmlXPathErrMemory(ctxt);
        return(-1);
    }

    result->type = XPATH_STRING;
    result->as.string = string;
    result->isCopy = isCopy;

    return(0);
}

/**
 * xmlXPathNodeValHash:
 * @node:  a node pointer
 *
 * Function computing the beginning of the string value of the node,
 * used to speed up comparisons
 *
 * Returns an int usable as a hash
 */
static unsigned int
xmlXPathNodeValHash(xmlNodePtr node) {
    int len = 2;
    const xmlChar * string = NULL;
    xmlNodePtr tmp = NULL;
    unsigned int ret = 0;

    if (node == NULL)
	return(0);

    if (node->type == XML_DOCUMENT_NODE) {
	tmp = xmlDocGetRootElement((xmlDocPtr) node);
	if (tmp == NULL)
	    node = node->children;
	else
	    node = tmp;

	if (node == NULL)
	    return(0);
    }

    switch (node->type) {
	case XML_COMMENT_NODE:
	case XML_PI_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_TEXT_NODE:
	    string = node->content;
	    if (string == NULL)
		return(0);
	    if (string[0] == 0)
		return(0);
	    return(string[0] + (string[1] << 8));
	case XML_NAMESPACE_DECL:
	    string = ((xmlNsPtr)node)->href;
	    if (string == NULL)
		return(0);
	    if (string[0] == 0)
		return(0);
	    return(string[0] + (string[1] << 8));
	case XML_ATTRIBUTE_NODE:
	    tmp = ((xmlAttrPtr) node)->children;
	    break;
	case XML_ELEMENT_NODE:
	    tmp = node->children;
	    break;
	default:
	    return(0);
    }
    while (tmp != NULL) {
	switch (tmp->type) {
	    case XML_CDATA_SECTION_NODE:
	    case XML_TEXT_NODE:
		string = tmp->content;
		break;
	    default:
                string = NULL;
		break;
	}
	if ((string != NULL) && (string[0] != 0)) {
	    if (len == 1) {
		return(ret + (string[0] << 8));
	    }
	    if (string[1] == 0) {
		len = 1;
		ret = string[0];
	    } else {
		return(string[0] + (string[1] << 8));
	    }
	}
	/*
	 * Skip to next node
	 */
        if ((tmp->children != NULL) &&
            (tmp->type != XML_DTD_NODE) &&
            (tmp->type != XML_ENTITY_REF_NODE) &&
            (tmp->children->type != XML_ENTITY_DECL)) {
            tmp = tmp->children;
            continue;
	}
	if (tmp == node)
	    break;

	if (tmp->next != NULL) {
	    tmp = tmp->next;
	    continue;
	}

	do {
	    tmp = tmp->parent;
	    if (tmp == NULL)
		break;
	    if (tmp == node) {
		tmp = NULL;
		break;
	    }
	    if (tmp->next != NULL) {
		tmp = tmp->next;
		break;
	    }
	} while (tmp != NULL);
    }
    return(ret);
}

/**
 * xmlXPathStringHash:
 * @string:  a string
 *
 * Function computing the beginning of the string value of the node,
 * used to speed up comparisons
 *
 * Returns an int usable as a hash
 */
static unsigned int
xmlXPathStringHash(const xmlChar * string) {
    if (string == NULL)
	return(0);
    if (string[0] == 0)
	return(0);
    return(string[0] + (string[1] << 8));
}

/**
 * xmlXPathCompareNodeSetValue:
 * @ctxt:  the XPath Parser context
 * @less:  less than (1) or greater than (0)
 * @strict:  is the comparison strict
 * @arg:  the node set
 * @f:  the value
 *
 * Implement the compare operation between a nodeset and a number
 *     @ns < @val    (1, 1, ...
 *     @ns <= @val   (1, 0, ...
 *     @ns > @val    (0, 1, ...
 *     @ns >= @val   (0, 0, ...
 *
 * If one object to be compared is a node-set and the other is a number,
 * then the comparison will be true if and only if there is a node in the
 * node-set such that the result of performing the comparison on the number
 * to be compared and on the result of converting the string-value of that
 * node to a number using the number function is true.
 *
 * Returns 0 or 1 depending on the results of the test.
 */
static int
xmlXPathCompareNodeSetValue(xmlXPathContextPtr ctxt, int less, int strict,
	                    const xmlNodeSet *ns, xmlXPathItem *item) {
    int i, ret = 0;
    double val2;

    val2 = xmlXPathItemToNumber(ctxt, item);

    for (i = 0; i < ns->nodeNr; i++) {
        double val1 = xmlXPathNodeToNumberInternal(ctxt, ns->nodeTab[i]);

        if (less) {
            if (strict)
                ret = (val1 < val2);
            else
                ret = (val1 <= val2);
        } else {
            if (strict)
                ret = (val1 > val2);
            else
                ret = (val1 >= val2);
        }
        if (ret)
            break;
    }

    return(ret);
}

/**
 * xmlXPathCompareNodeSets:
 * @less:  less than (1) or greater than (0)
 * @strict:  is the comparison strict
 * @arg1:  the first node set object
 * @arg2:  the second node set object
 *
 * Implement the compare operation on nodesets:
 *
 * If both objects to be compared are node-sets, then the comparison
 * will be true if and only if there is a node in the first node-set
 * and a node in the second node-set such that the result of performing
 * the comparison on the string-values of the two nodes is true.
 * ....
 * When neither object to be compared is a node-set and the operator
 * is <=, <, >= or >, then the objects are compared by converting both
 * objects to numbers and comparing the numbers according to IEEE 754.
 * ....
 * The number function converts its argument to a number as follows:
 *  - a string that consists of optional whitespace followed by an
 *    optional minus sign followed by a Number followed by whitespace
 *    is converted to the IEEE 754 number that is nearest (according
 *    to the IEEE 754 round-to-nearest rule) to the mathematical value
 *    represented by the string; any other string is converted to NaN
 *
 * Conclusion all nodes need to be converted first to their string value
 * and then the comparison must be done when possible
 */
static int
xmlXPathCompareNodeSets(xmlXPathContextPtr ctxt, int less, int strict,
	                const xmlNodeSet *ns1, const xmlNodeSet *ns2) {
    int i, j, init = 0;
    double *values2;
    int ret = 0;

    if (ns1->nodeNr <= 0) {
	return(0);
    }
    if (ns2->nodeNr <= 0) {
	return(0);
    }

    values2 = (double *) xmlMalloc(ns2->nodeNr * sizeof(double));
    if (values2 == NULL) {
        xmlXPathErrMemory(ctxt);
	return(0);
    }
    for (i = 0;i < ns1->nodeNr;i++) {
        double val1;

	val1 = xmlXPathNodeToNumberInternal(ctxt, ns1->nodeTab[i]);
	if (xmlXPathIsNaN(val1))
	    continue;

	for (j = 0;j < ns2->nodeNr;j++) {
            double val2;

	    if (init == 0) {
		values2[j] = xmlXPathNodeToNumberInternal(ctxt,
                                                          ns2->nodeTab[j]);
	    }
            val2 = values2[j];

            if (less) {
                if (strict)
                    ret = (val1 < val2);
                else
                    ret = (val1 <= val2);
            } else {
                if (strict)
                    ret = (val1 > val2);
                else
                    ret = (val1 >= val2);
            }
	    if (ret)
		break;
	}

	if (ret)
	    break;
	init = 1;
    }

    xmlFree(values2);
    return(ret);
}

/**
 * xmlXPathEqualNodeSetString:
 * @arg:  the nodeset object argument
 * @str:  the string to compare to.
 * @neq:  flag to show whether for '=' (0) or '!=' (1)
 *
 * Implement the equal operation on XPath objects content: @arg1 == @arg2
 * If one object to be compared is a node-set and the other is a string,
 * then the comparison will be true if and only if there is a node in
 * the node-set such that the result of performing the comparison on the
 * string-value of the node and the other string is true.
 *
 * Returns 0 or 1 depending on the results of the test.
 */
static int
xmlXPathEqualNodeSetString(xmlXPathContextPtr ctxt,
                           const xmlNodeSet *ns, const xmlChar * str, int neq)
{
    int i;
    xmlChar *str2;
    unsigned int hash;

    if (str == NULL)
        return (0);
    /*
     * A NULL nodeset compared with a string is always false
     * (since there is no node equal, and no node not equal)
     */
    if (ns->nodeNr <= 0)
        return (0);
    hash = xmlXPathStringHash(str);
    for (i = 0; i < ns->nodeNr; i++) {
        if (xmlXPathNodeValHash(ns->nodeTab[i]) == hash) {
            str2 = xmlNodeGetContent(ns->nodeTab[i]);
            if (str2 == NULL) {
                xmlXPathErrMemory(ctxt);
                return(0);
            }
            if (xmlStrEqual(str, str2)) {
                xmlFree(str2);
		if (neq)
		    continue;
                return (1);
            } else if (neq) {
		xmlFree(str2);
		return (1);
	    }
            xmlFree(str2);
        } else if (neq)
	    return (1);
    }
    return (0);
}

/**
 * xmlXPathEqualNodeSetFloat:
 * @arg:  the nodeset object argument
 * @f:  the float to compare to
 * @neq:  flag to show whether to compare '=' (0) or '!=' (1)
 *
 * Implement the equal operation on XPath objects content: @arg1 == @arg2
 * If one object to be compared is a node-set and the other is a number,
 * then the comparison will be true if and only if there is a node in
 * the node-set such that the result of performing the comparison on the
 * number to be compared and on the result of converting the string-value
 * of that node to a number using the number function is true.
 *
 * Returns 0 or 1 depending on the results of the test.
 */
static int
xmlXPathEqualNodeSetFloat(xmlXPathContextPtr ctxt,
    const xmlNodeSet *ns, double f, int neq) {
    int i, ret = 0;

    for (i = 0; i < ns->nodeNr; i++) {
        double val2 = xmlXPathNodeToNumberInternal(ctxt, ns->nodeTab[i]);

        if (neq)
            ret = (f != val2);
        else
            ret = (f == val2);
        if (ret)
            break;
    }

    return(ret);
}


/**
 * xmlXPathEqualNodeSets:
 * @arg1:  first nodeset object argument
 * @arg2:  second nodeset object argument
 * @neq:   flag to show whether to test '=' (0) or '!=' (1)
 *
 * Implement the equal / not equal operation on XPath nodesets:
 * @arg1 == @arg2  or  @arg1 != @arg2
 * If both objects to be compared are node-sets, then the comparison
 * will be true if and only if there is a node in the first node-set and
 * a node in the second node-set such that the result of performing the
 * comparison on the string-values of the two nodes is true.
 *
 * (needless to say, this is a costly operation)
 *
 * Returns 0 or 1 depending on the results of the test.
 */
static int
xmlXPathEqualNodeSets(xmlXPathContextPtr ctxt, const xmlNodeSet *ns1,
                      const xmlNodeSet *ns2, int neq) {
    int i, j;
    unsigned int *hashs1;
    unsigned int *hashs2;
    xmlChar **values1;
    xmlChar **values2;
    int ret = 0;

    if (ns1->nodeNr <= 0)
	return(0);
    if (ns2->nodeNr <= 0)
	return(0);

    /*
     * for equal, check if there is a node pertaining to both sets
     */
    if (neq == 0)
	for (i = 0;i < ns1->nodeNr;i++)
	    for (j = 0;j < ns2->nodeNr;j++)
		if (ns1->nodeTab[i] == ns2->nodeTab[j])
		    return(1);

    values1 = (xmlChar **) xmlMalloc(ns1->nodeNr * sizeof(xmlChar *));
    if (values1 == NULL) {
        xmlXPathErrMemory(ctxt);
	return(0);
    }
    hashs1 = (unsigned int *) xmlMalloc(ns1->nodeNr * sizeof(unsigned int));
    if (hashs1 == NULL) {
        xmlXPathErrMemory(ctxt);
	xmlFree(values1);
	return(0);
    }
    memset(values1, 0, ns1->nodeNr * sizeof(xmlChar *));
    values2 = (xmlChar **) xmlMalloc(ns2->nodeNr * sizeof(xmlChar *));
    if (values2 == NULL) {
        xmlXPathErrMemory(ctxt);
	xmlFree(hashs1);
	xmlFree(values1);
	return(0);
    }
    hashs2 = (unsigned int *) xmlMalloc(ns2->nodeNr * sizeof(unsigned int));
    if (hashs2 == NULL) {
        xmlXPathErrMemory(ctxt);
	xmlFree(hashs1);
	xmlFree(values1);
	xmlFree(values2);
	return(0);
    }
    memset(values2, 0, ns2->nodeNr * sizeof(xmlChar *));
    for (i = 0;i < ns1->nodeNr;i++) {
	hashs1[i] = xmlXPathNodeValHash(ns1->nodeTab[i]);
	for (j = 0;j < ns2->nodeNr;j++) {
	    if (i == 0)
		hashs2[j] = xmlXPathNodeValHash(ns2->nodeTab[j]);
	    if (hashs1[i] != hashs2[j]) {
		if (neq) {
		    ret = 1;
		    break;
		}
	    }
	    else {
		if (values1[i] == NULL) {
		    values1[i] = xmlNodeGetContent(ns1->nodeTab[i]);
                    if (values1[i] == NULL)
                        xmlXPathErrMemory(ctxt);
                }
		if (values2[j] == NULL) {
		    values2[j] = xmlNodeGetContent(ns2->nodeTab[j]);
                    if (values2[j] == NULL)
                        xmlXPathErrMemory(ctxt);
                }
		ret = xmlStrEqual(values1[i], values2[j]) ^ neq;
		if (ret)
		    break;
	    }
	}
	if (ret)
	    break;
    }
    for (i = 0;i < ns1->nodeNr;i++)
	if (values1[i] != NULL)
	    xmlFree(values1[i]);
    for (j = 0;j < ns2->nodeNr;j++)
	if (values2[j] != NULL)
	    xmlFree(values2[j]);
    xmlFree(values1);
    xmlFree(values2);
    xmlFree(hashs1);
    xmlFree(hashs2);
    return(ret);
}

static int
xmlXPathEqualValuesInternal(xmlXPathContextPtr ctxt,
                            xmlXPathItem *item1, xmlXPathItem *item2,
                            int neq) {
    int ret = 0;

    switch (item1->type) {
        case XPATH_BOOLEAN:
            ret = (item1->as.boolean == xmlXPathItemToBoolean(ctxt, item2));
	    break;
        case XPATH_NUMBER:
	    switch (item2->type) {
		case XPATH_BOOLEAN: {
                    int bool1 = (item1->as.number != 0.0);
                    ret = (bool1 == item2->as.boolean);
		    break;
                }
		case XPATH_STRING: {
                    double val2 = xmlXPathStringEvalNumber(item2->as.string);
		    ret = (item1->as.number == val2);
                    xmlXPathItemReleaseString(item2);
                    break;
                }
		case XPATH_NUMBER:
		    ret = (item1->as.number == item2->as.number);
		    break;
                case XPATH_NODESET:
                case XPATH_XSLT_TREE:
                    ret = xmlXPathEqualNodeSetFloat(ctxt, &item2->as.nodeset,
                                                    item1->as.number, neq);
                    neq = 0;
                    xmlXPathItemReleaseNodeSet(ctxt, item2);
                    break;
                default:
		    break;
	    }
	    break;
        case XPATH_STRING:
	    switch (item2->type) {
		case XPATH_BOOLEAN: {
                    int bool1 = ((item1->as.string != NULL) &&
                                 (item1->as.string[0] != 0));
                    ret = (bool1 == item2->as.boolean);
		    break;
                }
		case XPATH_STRING:
		    ret = xmlStrEqual(item1->as.string, item2->as.string);
                    xmlXPathItemReleaseString(item2);
		    break;
		case XPATH_NUMBER: {
                    double val1 = xmlXPathStringEvalNumber(item1->as.string);
		    ret = (val1 == item2->as.number);
                    break;
                }
                case XPATH_NODESET:
                case XPATH_XSLT_TREE:
                    ret = xmlXPathEqualNodeSetString(ctxt, &item2->as.nodeset,
                                                     item1->as.string, neq);
                    neq = 0;
                    xmlXPathItemReleaseNodeSet(ctxt, item2);
                    break;
                default:
		    break;
	    }
            xmlXPathItemReleaseString(item1);
	    break;
        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
	    switch (item2->type) {
		case XPATH_BOOLEAN: {
                    int bool1 = (item1->as.nodeset.nodeNr > 0);
                    ret = (bool1 == item2->as.boolean);
		    break;
                }
		case XPATH_STRING:
                    ret = xmlXPathEqualNodeSetString(ctxt, &item1->as.nodeset,
                                                     item2->as.string, neq);
                    neq = 0;
                    xmlXPathItemReleaseString(item2);
		    break;
		case XPATH_NUMBER:
                    ret = xmlXPathEqualNodeSetFloat(ctxt, &item1->as.nodeset,
                                                    item2->as.number, neq);
                    neq = 0;
                    break;
                case XPATH_NODESET:
                case XPATH_XSLT_TREE:
		    ret = xmlXPathEqualNodeSets(ctxt, &item1->as.nodeset,
                                                &item2->as.nodeset, neq);
                    neq = 0;
                    xmlXPathItemReleaseNodeSet(ctxt, item2);
                    break;
                default:
		    break;
	    }
            xmlXPathItemReleaseNodeSet(ctxt, item1);
	    break;
        default:
	    break;
    }

    if (neq)
        ret = !ret;

    return(ret);
}

/**
 * xmlXPathEqualValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the equal operation on XPath objects content: @arg1 == @arg2
 *
 * Returns 0 or 1 depending on the results of the test.
 */
int
xmlXPathEqualValues(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr arg1, arg2;
    int ret = 0;

    if ((ctxt == NULL) || (ctxt->context == NULL))
        return(0);

    xpctxt = ctxt->context;

    arg2 = valuePop(ctxt);
    arg1 = valuePop(ctxt);
    if ((arg1 == NULL) || (arg2 == NULL)) {
        xmlXPathCErr(xpctxt, XPATH_INVALID_OPERAND);
    } else {
        xmlXPathItem item1, item2;

        xmlXPathItemFromObj(xpctxt, &item1, arg1, 1);
        xmlXPathItemFromObj(xpctxt, &item2, arg2, 1);
        ret = xmlXPathEqualValuesInternal(xpctxt, &item1, &item2, 0);
    }

    xmlXPathReleaseObject(xpctxt, arg1);
    xmlXPathReleaseObject(xpctxt, arg2);
    return(ret);
}

/**
 * xmlXPathNotEqualValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the not-equal operation on XPath objects content:
 * @arg1 != @arg2
 *
 * Returns 0 or 1 depending on the results of the test.
 */
int
xmlXPathNotEqualValues(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr arg1, arg2;
    int ret = 0;

    if ((ctxt == NULL) || (ctxt->context == NULL))
        return(0);

    xpctxt = ctxt->context;

    arg2 = valuePop(ctxt);
    arg1 = valuePop(ctxt);
    if ((arg1 == NULL) || (arg2 == NULL)) {
        xmlXPathCErr(xpctxt, XPATH_INVALID_OPERAND);
    } else {
        xmlXPathItem item1, item2;

        xmlXPathItemFromObj(xpctxt, &item1, arg1, 1);
        xmlXPathItemFromObj(xpctxt, &item2, arg2, 1);
        ret = xmlXPathEqualValuesInternal(xpctxt, &item1, &item2, 1);
    }

    xmlXPathReleaseObject(xpctxt, arg1);
    xmlXPathReleaseObject(xpctxt, arg2);
    return(ret);
}

static int
xmlXPathCompareValuesInternal(xmlXPathContextPtr ctxt,
                              xmlXPathItem *item1, xmlXPathItem *item2,
                              int less, int strict) {
    int ret = 0;

    if ((item1->type == XPATH_NODESET) || (item1->type == XPATH_XSLT_TREE)) {
        if ((item2->type == XPATH_NODESET) || (item2->type == XPATH_XSLT_TREE)) {
	    ret = xmlXPathCompareNodeSets(ctxt, less, strict,
                                          &item1->as.nodeset,
                                          &item2->as.nodeset);
            xmlXPathItemReleaseNodeSet(ctxt, item1);
            xmlXPathItemReleaseNodeSet(ctxt, item2);
        } else {
            ret = xmlXPathCompareNodeSetValue(ctxt, less, strict,
                                              &item1->as.nodeset, item2);
            xmlXPathItemReleaseNodeSet(ctxt, item1);
        }
    } else {
        if ((item2->type == XPATH_NODESET) || (item2->type == XPATH_XSLT_TREE)) {
            ret = xmlXPathCompareNodeSetValue(ctxt, !less, strict,
                                              &item2->as.nodeset, item1);
            xmlXPathItemReleaseNodeSet(ctxt, item2);
        } else {
            double val1 = xmlXPathItemToNumber(ctxt, item1);
            double val2 = xmlXPathItemToNumber(ctxt, item2);

            if (less) {
                if (strict)
                    ret = (val1 < val2);
                else
                    ret = (val1 <= val2);
            } else {
                if (strict)
                    ret = (val1 > val2);
                else
                    ret = (val1 >= val2);
            }
        }
    }

    return(ret);
}

/**
 * xmlXPathCompareValues:
 * @ctxt:  the XPath Parser context
 * @less:  less than (1) or greater than (0)
 * @strict:  is the comparison strict
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the compare operation on XPath objects:
 *     @arg1 < @arg2    (1, 1, ...
 *     @arg1 <= @arg2   (1, 0, ...
 *     @arg1 > @arg2    (0, 1, ...
 *     @arg1 >= @arg2   (0, 0, ...
 *
 * When neither object to be compared is a node-set and the operator is
 * <=, <, >=, >, then the objects are compared by converted both objects
 * to numbers and comparing the numbers according to IEEE 754. The <
 * comparison will be true if and only if the first number is less than the
 * second number. The <= comparison will be true if and only if the first
 * number is less than or equal to the second number. The > comparison
 * will be true if and only if the first number is greater than the second
 * number. The >= comparison will be true if and only if the first number
 * is greater than or equal to the second number.
 *
 * Returns 1 if the comparison succeeded, 0 if it failed
 */
int
xmlXPathCompareValues(xmlXPathParserContextPtr ctxt, int less, int strict) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr arg1, arg2;
    int ret = 0;

    if ((ctxt == NULL) || (ctxt->context == NULL))
        return(0);

    xpctxt = ctxt->context;

    arg2 = valuePop(ctxt);
    arg1 = valuePop(ctxt);
    if ((arg1 == NULL) || (arg2 == NULL)) {
        xmlXPathCErr(xpctxt, XPATH_INVALID_OPERAND);
    } else {
        xmlXPathItem item1, item2;

        xmlXPathItemFromObj(xpctxt, &item1, arg1, 1);
        xmlXPathItemFromObj(xpctxt, &item2, arg2, 1);
        ret = xmlXPathCompareValuesInternal(xpctxt, &item1, &item2,
                                            less, strict);
    }

    xmlXPathReleaseObject(xpctxt, arg1);
    xmlXPathReleaseObject(xpctxt, arg2);
    return(ret);
}

/**
 * xmlXPathValueFlipSign:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the unary - operation on an XPath object
 * The numeric operators convert their operands to numbers as if
 * by calling the number function.
 */
void
xmlXPathValueFlipSign(xmlXPathParserContextPtr ctxt) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return;

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval = -ctxt->value->floatval;
}

/**
 * xmlXPathAddValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the add operation on XPath objects:
 * The numeric operators convert their operands to numbers as if
 * by calling the number function.
 */
void
xmlXPathAddValues(xmlXPathParserContextPtr ctxt) {
    double val;

    if (ctxt == NULL)
        return;

    val = xmlXPathPopNumber(ctxt);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval += val;
}

/**
 * xmlXPathSubValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the subtraction operation on XPath objects:
 * The numeric operators convert their operands to numbers as if
 * by calling the number function.
 */
void
xmlXPathSubValues(xmlXPathParserContextPtr ctxt) {
    double val;

    if (ctxt == NULL)
        return;

    val = xmlXPathPopNumber(ctxt);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval -= val;
}

/**
 * xmlXPathMultValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the multiply operation on XPath objects:
 * The numeric operators convert their operands to numbers as if
 * by calling the number function.
 */
void
xmlXPathMultValues(xmlXPathParserContextPtr ctxt) {
    double val;

    if (ctxt == NULL)
        return;

    val = xmlXPathPopNumber(ctxt);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval *= val;
}

/**
 * xmlXPathDivValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the div operation on XPath objects @arg1 / @arg2:
 * The numeric operators convert their operands to numbers as if
 * by calling the number function.
 */
ATTRIBUTE_NO_SANITIZE("float-divide-by-zero")
void
xmlXPathDivValues(xmlXPathParserContextPtr ctxt) {
    double val;

    if (ctxt == NULL)
        return;

    val = xmlXPathPopNumber(ctxt);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval /= val;
}

/**
 * xmlXPathModValues:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the mod operation on XPath objects: @arg1 / @arg2
 * The numeric operators convert their operands to numbers as if
 * by calling the number function.
 */
void
xmlXPathModValues(xmlXPathParserContextPtr ctxt) {
    double val;

    if (ctxt == NULL)
        return;

    val = xmlXPathPopNumber(ctxt);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval = fmod(ctxt->value->floatval, val);
}

/************************************************************************
 *									*
 *		The traversal functions					*
 *									*
 ************************************************************************/

/*
 * xmlXPathNodeSetMergeFunction:
 * Used for merging node sets in xmlXPathCollectAndTest().
 */
typedef xmlNodeSetPtr
(*xmlXPathNodeSetMergeFunction)(xmlNodeSetPtr set1, xmlNodeSetPtr set2,
                                xmlXPathEvalMode mode);

typedef struct {
    union {
        xmlNodePtr node;
        xmlNodePtr *nodes;
    } as;

    int index;
    int hasNodes;
} xmlIter;

typedef xmlNodePtr
(*xmlIterNextFunc)(xmlIter *ctxt, xmlNodePtr cur);

typedef xmlNodePtr
(*xmlIterStartFunc)(xmlIter *ctxt, xmlNodePtr cur, int reverse,
                    xmlIterNextFunc *next);

static xmlNodePtr
xmlIterEnd(xmlIter *ctxt, xmlNodePtr cur) {
    (void) ctxt;
    (void) cur;

    return(NULL);
}

static xmlNodePtr
xmlIterNextSibling(xmlIter *ctxt, xmlNodePtr cur) {
    (void) ctxt;

    return(cur->next);
}

static xmlNodePtr
xmlIterPrevSibling(xmlIter *ctxt, xmlNodePtr cur) {
    (void) ctxt;

    return(cur->prev);
}

static xmlNodePtr
xmlIterNextSiblingUntil(xmlIter *ctxt, xmlNodePtr cur) {
    xmlNodePtr node = cur->next;

    if (node == ctxt->as.node)
        return(NULL);
    return(node);
}

static xmlNodePtr
xmlIterPrevSiblingUntil(xmlIter *ctxt, xmlNodePtr cur) {
    xmlNodePtr node = cur->prev;

    if (node == ctxt->as.node)
        return(NULL);
    return(node);
}

static xmlNodePtr
xmlIterNextParent(xmlIter *ctxt, xmlNodePtr cur) {
    (void) ctxt;

    return(cur->parent);
}

static xmlNodePtr
xmlIterNextParentNs(xmlIter *ctxt, xmlNodePtr cur) {
    (void) ctxt;

    if (cur->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) cur;

        return((xmlNodePtr) ns->next);
    }

    return(cur->parent);
}

static xmlNodePtr
xmlIterNextDescendant(xmlIter *ctxt, xmlNodePtr cur) {
    if ((cur->children != NULL) && ((1 << cur->type) & TYPE_MASK_NODE))
        return(cur->children);

    while (cur->next == NULL) {
        cur = cur->parent;

        if ((cur == NULL) || (cur == ctxt->as.node))
            return(NULL);
    }

    return(cur->next);
}

static xmlNodePtr
xmlIterPrevDescendant(xmlIter *ctxt, xmlNodePtr cur) {
    if (cur->prev == NULL) {
        cur = cur->parent;
        if ((cur == NULL) || (cur == ctxt->as.node))
            return(NULL);

        return(cur);
    }

    cur = cur->prev;
    if (cur == ctxt->as.node)
        return(NULL);

    while ((cur->last != NULL) && ((1 << cur->last->type) & TYPE_MASK_NODE)) {
        if (cur == ctxt->as.node)
            return(NULL);
        cur = cur->last;
    }

    return(cur);
}

static xmlNodePtr
xmlIterNextPreceding(xmlIter *ctxt, xmlNodePtr cur) {
    if (cur->prev == NULL) {
        cur = cur->parent;
        if (cur == NULL)
            return(NULL);
        if (cur != ctxt->as.node)
            return(cur);

        /* Skip ancestors */
        while (cur->prev == NULL) {
            cur = cur->parent;
            if (cur == NULL)
                return(NULL);
        }
        ctxt->as.node = cur->parent;
    }

    cur = cur->prev;

    while ((cur->last != NULL) && ((1 << cur->last->type) & TYPE_MASK_NODE))
        cur = cur->last;

    return(cur);
}

static xmlNodePtr
xmlIterPrevPreceding(xmlIter *ctxt, xmlNodePtr cur) {
    int i;

    if ((cur->children != NULL) && ((1 << cur->type) & TYPE_MASK_NODE))
        return(cur->children);

    while (cur->next == NULL) {
        cur = cur->parent;

        if ((cur == NULL) || (cur == ctxt->as.node))
            return(NULL);
    }

    i = ctxt->index;
    if (cur->next != ctxt->as.nodes[i])
        return(cur->next);

    if (i <= 0)
        return(NULL);

    i -= 1;
    ctxt->index = i;
    cur = ctxt->as.nodes[i];

    while (cur->prev != NULL)
        cur = cur->prev;

    return(cur);
}

static xmlNodePtr
xmlIterNextTable(xmlIter *ctxt, xmlNodePtr cur) {
    (void) cur;

    if (ctxt->index == 0)
        return(NULL);

    return(ctxt->as.nodes[--ctxt->index]);
}

static xmlNodePtr
xmlIterStartSelf(xmlIter *ctxt, xmlNodePtr node, int reverse,
                 xmlIterNextFunc *next) {
    (void) reverse;

    ctxt->hasNodes = 0;
    *next = xmlIterEnd;
    return(node);
}

static xmlNodePtr
xmlIterStartChild(xmlIter *ctxt, xmlNodePtr node, int reverse,
                  xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if ((node->type == XML_ATTRIBUTE_NODE) ||
        (node->type == XML_NAMESPACE_DECL))
        return(NULL);

    if (!reverse) {
        *next = xmlIterNextSibling;
        return(node->children);
    } else {
        *next = xmlIterPrevSibling;
        return(node->last);
    }
}

static xmlNodePtr
xmlIterStartAttribute(xmlIter *ctxt, xmlNodePtr node, int reverse,
                      xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if (node->type != XML_ELEMENT_NODE)
        return(NULL);

    if (!reverse) {
        *next = xmlIterNextSibling;
        return((xmlNodePtr) node->properties);
    } else {
        xmlAttrPtr attr = node->properties;

        if (attr != NULL) {
            while (attr->next != NULL)
                attr = attr->next;
        }

        *next = xmlIterPrevSibling;
        return((xmlNodePtr) attr);
    }
}

static xmlNodePtr
xmlIterStartDescendant(xmlIter *ctxt, xmlNodePtr node, int reverse,
                       xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if ((node->type == XML_ATTRIBUTE_NODE) ||
        (node->type == XML_NAMESPACE_DECL))
        return(NULL);

    if (!reverse) {
        *next = xmlIterNextDescendant;
        ctxt->as.node = node;
        return(node->children);
    } else {
        xmlNodePtr cur;

        cur = node;
        while ((cur->last != NULL) &&
               ((1 << cur->last->type) & TYPE_MASK_NODE))
            cur = cur->last;

        ctxt->as.node = node;
        *next = xmlIterPrevDescendant;
        return(cur);
    }
}

static xmlNodePtr
xmlIterStartDescendantOrSelf(xmlIter *ctxt, xmlNodePtr node, int reverse,
                             xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if ((node->type == XML_ATTRIBUTE_NODE) ||
        (node->type == XML_NAMESPACE_DECL)) {
        *next = xmlIterEnd;
        return(node);
    }

    if (!reverse) {
        *next = xmlIterNextDescendant;
        ctxt->as.node = node;
        return(node);
    } else {
        xmlNodePtr cur;

        if (node->prev != NULL)
            ctxt->as.node = node->prev;
        else
            ctxt->as.node = node->parent;

        cur = node;
        while ((cur->last != NULL) &&
               ((1 << cur->last->type) & TYPE_MASK_NODE))
            cur = cur->last;

        *next = xmlIterPrevDescendant;
        return(cur);
    }
}

static xmlNodePtr
xmlIterStartParent(xmlIter *ctxt, xmlNodePtr node, int reverse,
                   xmlIterNextFunc *next) {
    (void) reverse;

    ctxt->hasNodes = 0;

    *next = xmlIterEnd;

    if (node->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node;

        return((xmlNodePtr) ns->next);
    } else {
        return(node->parent);
    }
}

static xmlNodePtr
xmlIterStartAncestor(xmlIter *ctxt, xmlNodePtr node, int reverse,
                     xmlIterNextFunc *next) {
    xmlNodePtr parent;

    ctxt->hasNodes = 0;

    if (node->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node;

        parent = (xmlNodePtr) ns->next;
    } else {
        parent = node->parent;
    }

    if (!reverse) {
        *next = xmlIterNextParent;
        return(parent);
    } else {
        xmlNodePtr cur;
        xmlNodePtr *nodes = NULL;
        int numNodes, i;

        if (parent == NULL)
            return(NULL);

        cur = parent;
        numNodes = 0;

        if (cur->parent != NULL) {
            ctxt->hasNodes = 1;

            while (cur->parent != NULL) {
                numNodes += 1;
                cur = cur->parent;
            }

            nodes = xmlMalloc(numNodes * sizeof(nodes[0]));
            if (nodes == NULL) {
                ctxt->as.nodes = NULL;
                return(NULL);
            }

            i = 0;
            cur = parent;
            while (cur->parent != NULL) {
                nodes[i] = cur;
                i += 1;
                cur = cur->parent;
            }
        }

        ctxt->as.nodes = nodes;
        ctxt->index = numNodes;

        *next = xmlIterNextTable;
        return(cur);
    }
}

static xmlNodePtr
xmlIterStartAncestorOrSelf(xmlIter *ctxt, xmlNodePtr node, int reverse,
                           xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if (!reverse) {
        if (node->type == XML_NAMESPACE_DECL)
            *next = xmlIterNextParentNs;
        else
            *next = xmlIterNextParent;

        return(node);
    } else {
        xmlNodePtr parent, cur;
        xmlNodePtr *nodes = NULL;
        int numNodes, i;

        if (node->type == XML_NAMESPACE_DECL) {
            xmlNsPtr ns = (xmlNsPtr) node;

            parent = (xmlNodePtr) ns->next;
        } else {
            parent = node->parent;
        }

        numNodes = 0;

        if (parent == NULL) {
            cur = node;
        } else {
            ctxt->hasNodes = 1;

            cur = parent;
            numNodes += 1;

            while (cur->parent != NULL) {
                numNodes += 1;
                cur = cur->parent;
            }

            nodes = xmlMalloc(numNodes * sizeof(nodes[0]));
            if (nodes == NULL) {
                ctxt->as.nodes = NULL;
                return(NULL);
            }

            nodes[0] = node;
            i = 1;
            cur = parent;
            while (cur->parent != NULL) {
                nodes[i] = cur;
                i += 1;
                cur = cur->parent;
            }

        }

        ctxt->as.nodes = nodes;
        ctxt->index = numNodes;

        *next = xmlIterNextTable;
        return(cur);
    }
}

static xmlNodePtr
xmlIterStartFollowingSibling(xmlIter *ctxt, xmlNodePtr node, int reverse,
                             xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if ((node->type == XML_ATTRIBUTE_NODE) ||
        (node->type == XML_NAMESPACE_DECL) ||
        (node->type == XML_DOCUMENT_NODE) ||
        (node->type == XML_HTML_DOCUMENT_NODE))
        return(NULL);

    if (!reverse) {
        *next = xmlIterNextSibling;
        return(node->next);
    } else {
        xmlNodePtr last;

        if (node->parent == NULL)
            return(NULL);
        last = node->parent->last;
        if (node == last)
            return(NULL);

        ctxt->as.node = node;
        *next = xmlIterPrevSiblingUntil;
        return(last);
    }
}

static xmlNodePtr
xmlIterStartPrecedingSibling(xmlIter *ctxt, xmlNodePtr node, int reverse,
                             xmlIterNextFunc *next) {
    ctxt->hasNodes = 0;

    if ((node->type == XML_ATTRIBUTE_NODE) ||
        (node->type == XML_NAMESPACE_DECL) ||
        (node->type == XML_DOCUMENT_NODE) ||
        (node->type == XML_HTML_DOCUMENT_NODE))
        return(NULL);

    if (!reverse) {
        *next = xmlIterPrevSibling;
        return(node->prev);
    } else {
        xmlNodePtr first;

        if (node->parent == NULL)
            return(NULL);
        first = node->parent->children;
        if (node == first)
            return(NULL);

        ctxt->as.node = node;
        *next = xmlIterNextSiblingUntil;
        return(first);
    }
}

static xmlNodePtr
xmlIterStartFollowing(xmlIter *ctxt, xmlNodePtr node, int reverse,
                      xmlIterNextFunc *next) {
    xmlNodePtr ancestor;

    ctxt->hasNodes = 0;

    if (node->type == XML_ATTRIBUTE_NODE) {
        ancestor = node->parent;
    } else if (node->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node;

        ancestor = (xmlNodePtr) ns->next;
    } else {
        ancestor = node;
    }

    if (!reverse) {
        while ((ancestor != NULL) &&
               (ancestor->type != XML_DOCUMENT_NODE) &&
               (ancestor->type != XML_HTML_DOCUMENT_NODE)) {
            if (ancestor->next != NULL) {
                *next = xmlIterNextDescendant;
                ctxt->as.node = (xmlNodePtr) ancestor->doc;
                return(ancestor->next);
            }

            ancestor = ancestor->parent;
        }

        return(NULL);
    } else {
        xmlNodePtr cur = NULL;

        while ((ancestor != NULL) &&
               (ancestor->type != XML_DOCUMENT_NODE) &&
               (ancestor->type != XML_HTML_DOCUMENT_NODE)) {
            if (ancestor->next != NULL)
                cur = ancestor->next;
            ancestor = ancestor->parent;
        }

        if (cur == NULL)
            return(NULL);

        while (cur->next != NULL)
            cur = cur->next;

        while ((cur->last != NULL) &&
               ((1 << cur->last->type) & TYPE_MASK_NODE))
            cur = cur->last;

        *next = xmlIterPrevDescendant;
        ctxt->as.node = node;
        return(cur);
    }
}

static xmlNodePtr
xmlIterStartPreceding(xmlIter *ctxt, xmlNodePtr node, int reverse,
                      xmlIterNextFunc *next) {
    xmlNodePtr ancestor, cur;

    ctxt->hasNodes = 0;

    if (node->type == XML_ATTRIBUTE_NODE) {
        ancestor = node->parent;
    } else if (node->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node;

        ancestor = (xmlNodePtr) ns->next;
    } else {
        ancestor = node;
    }

    if (!reverse) {
        while (ancestor->prev == NULL) {
            ancestor = ancestor->parent;
            if ((ancestor == NULL) ||
                (ancestor->type == XML_DOCUMENT_NODE) ||
                (ancestor->type == XML_HTML_DOCUMENT_NODE))
                return(NULL);
        }

        *next = xmlIterNextPreceding;
        ctxt->as.node = ancestor->parent;

        cur = ancestor->prev;

        while ((cur->last != NULL) &&
               ((1 << cur->last->type) & TYPE_MASK_NODE))
            cur = cur->last;

        return(cur);
    } else {
        xmlNodePtr *nodes;
        int numNodes, i;

        numNodes = 0;
        cur = ancestor;
        while (cur != NULL) {
            if (cur->prev != NULL)
                numNodes += 1;
            cur = cur->parent;
            if ((cur == NULL) ||
                (cur->type == XML_DOCUMENT_NODE) ||
                (cur->type == XML_HTML_DOCUMENT_NODE))
                break;
        }

        if (numNodes == 0)
            return(NULL);

        ctxt->hasNodes = 1;

        nodes = xmlMalloc(numNodes * sizeof(nodes[0]));
        if (nodes == NULL) {
            ctxt->as.nodes = NULL;
            return(NULL);
        }

        i = 0;
        cur = ancestor;
        while (cur != NULL) {
            if (cur->prev != NULL) {
                nodes[i] = cur;
                i += 1;
            }
            cur = cur->parent;
            if ((cur == NULL) ||
                (cur->type == XML_DOCUMENT_NODE) ||
                (cur->type == XML_HTML_DOCUMENT_NODE))
                break;
        }

        cur = nodes[numNodes - 1];
        while (cur->prev != NULL)
            cur = cur->prev;

        ctxt->as.nodes = nodes;
        ctxt->index = numNodes - 1;
        ctxt->hasNodes = 1;

        *next = xmlIterPrevPreceding;
        return(cur);
    }
}

static int
xmlIterCmpNsNodes(const void *v1, const void *v2) {
    const xmlNsPtr *ns1 = v1;
    const xmlNsPtr *ns2 = v2;

    return(xmlStrcmp((*ns2)->prefix, (*ns1)->prefix));
}

static int
xmlIterCmpNsNodesReverse(const void *v1, const void *v2) {
    const xmlNsPtr *ns1 = v1;
    const xmlNsPtr *ns2 = v2;

    return(xmlStrcmp((*ns1)->prefix, (*ns2)->prefix));
}

static xmlNodePtr
xmlIterStartNamespace(xmlIter *ctxt, xmlNodePtr node, int reverse,
                      xmlIterNextFunc *next) {
    xmlNsPtr *list;
    int i = 0;

    ctxt->hasNodes = 0;

    if (node->type != XML_ELEMENT_NODE)
        return(NULL);

    if (xmlGetNsListSafe(node->doc, node, &list) < 0) {
        ctxt->as.nodes = NULL;
        ctxt->hasNodes = 1;
        return(NULL);
    }
    if (list == NULL) {
        *next = xmlIterEnd;
        return((xmlNodePtr) xmlXPathXMLNamespace);
    }

    while (list[i] != NULL)
        i++;
    list[i] = (xmlNsPtr) xmlXPathXMLNamespace;

    qsort(list, i + 1, sizeof(list[0]),
          (reverse) ? xmlIterCmpNsNodesReverse : xmlIterCmpNsNodes);

    ctxt->hasNodes = 1;
    ctxt->as.nodes = (xmlNodePtr *) list;
    ctxt->index = i;

    *next = xmlIterNextTable;
    return((xmlNodePtr) list[i]);
}

static const xmlIterStartFunc xmlIterStart[14] = {
    NULL,
    xmlIterStartAncestor,
    xmlIterStartAncestorOrSelf,
    xmlIterStartAttribute,
    xmlIterStartChild,
    xmlIterStartDescendant,
    xmlIterStartDescendantOrSelf,
    xmlIterStartFollowing,
    xmlIterStartFollowingSibling,
    xmlIterStartNamespace,
    xmlIterStartParent,
    xmlIterStartPreceding,
    xmlIterStartPrecedingSibling,
    xmlIterStartSelf
};

/**
 * xmlXPathNextSelf:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "self" direction
 * The self axis contains just the context node itself
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextSelf(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (cur == NULL)
        return(ctxt->context->node);
    return(NULL);
}

/**
 * xmlXPathNextChild:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "child" direction
 * The child axis contains the children of the context node in document order.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextChild(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (cur == NULL) {
	if (ctxt->context->node == NULL) return(NULL);
	switch (ctxt->context->node->type) {
            case XML_ELEMENT_NODE:
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
            case XML_ENTITY_REF_NODE:
            case XML_ENTITY_NODE:
            case XML_PI_NODE:
            case XML_COMMENT_NODE:
            case XML_NOTATION_NODE:
            case XML_DTD_NODE:
		return(ctxt->context->node->children);
            case XML_DOCUMENT_NODE:
            case XML_DOCUMENT_TYPE_NODE:
            case XML_DOCUMENT_FRAG_NODE:
            case XML_HTML_DOCUMENT_NODE:
		return(((xmlDocPtr) ctxt->context->node)->children);
	    case XML_ELEMENT_DECL:
	    case XML_ATTRIBUTE_DECL:
	    case XML_ENTITY_DECL:
            case XML_ATTRIBUTE_NODE:
	    case XML_NAMESPACE_DECL:
	    case XML_XINCLUDE_START:
	    case XML_XINCLUDE_END:
		return(NULL);
	}
	return(NULL);
    }
    if ((cur->type == XML_DOCUMENT_NODE) ||
        (cur->type == XML_HTML_DOCUMENT_NODE))
	return(NULL);
    return(cur->next);
}

/**
 * xmlXPathNextDescendant:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "descendant" direction
 * the descendant axis contains the descendants of the context node in document
 * order; a descendant is a child or a child of a child and so on.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextDescendant(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (cur == NULL) {
	if (ctxt->context->node == NULL)
	    return(NULL);
	if ((ctxt->context->node->type == XML_ATTRIBUTE_NODE) ||
	    (ctxt->context->node->type == XML_NAMESPACE_DECL))
	    return(NULL);

        return(ctxt->context->node->children);
    }

    if (cur->type == XML_NAMESPACE_DECL)
        return(NULL);
    if (cur->children != NULL) {
	/*
	 * Do not descend on entities declarations
	 */
	if (cur->children->type != XML_ENTITY_DECL) {
	    cur = cur->children;
	    /*
	     * Skip DTDs
	     */
	    if (cur->type != XML_DTD_NODE)
		return(cur);
	}
    }

    if (cur == ctxt->context->node) return(NULL);

    while (cur->next != NULL) {
	cur = cur->next;
	if ((cur->type != XML_ENTITY_DECL) &&
	    (cur->type != XML_DTD_NODE))
	    return(cur);
    }

    do {
        cur = cur->parent;
	if (cur == NULL) break;
	if (cur == ctxt->context->node) return(NULL);
	if (cur->next != NULL) {
	    cur = cur->next;
	    return(cur);
	}
    } while (cur != NULL);
    return(cur);
}

/**
 * xmlXPathNextDescendantOrSelf:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "descendant-or-self" direction
 * the descendant-or-self axis contains the context node and the descendants
 * of the context node in document order; thus the context node is the first
 * node on the axis, and the first child of the context node is the second node
 * on the axis
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextDescendantOrSelf(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (cur == NULL)
        return(ctxt->context->node);

    if (ctxt->context->node == NULL)
        return(NULL);
    if ((ctxt->context->node->type == XML_ATTRIBUTE_NODE) ||
        (ctxt->context->node->type == XML_NAMESPACE_DECL))
        return(NULL);

    return(xmlXPathNextDescendant(ctxt, cur));
}

/**
 * xmlXPathNextParent:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "parent" direction
 * The parent axis contains the parent of the context node, if there is one.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextParent(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    /*
     * the parent of an attribute or namespace node is the element
     * to which the attribute or namespace node is attached
     * Namespace handling !!!
     */
    if (cur == NULL) {
	if (ctxt->context->node == NULL) return(NULL);
	switch (ctxt->context->node->type) {
            case XML_ELEMENT_NODE:
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
            case XML_ENTITY_REF_NODE:
            case XML_ENTITY_NODE:
            case XML_PI_NODE:
            case XML_COMMENT_NODE:
            case XML_NOTATION_NODE:
            case XML_DTD_NODE:
	    case XML_ELEMENT_DECL:
	    case XML_ATTRIBUTE_DECL:
	    case XML_XINCLUDE_START:
	    case XML_XINCLUDE_END:
	    case XML_ENTITY_DECL:
		if (ctxt->context->node->parent == NULL)
		    return(NULL);
		if ((ctxt->context->node->parent->type == XML_ELEMENT_NODE) &&
		    ((ctxt->context->node->parent->name[0] == ' ') ||
		     (xmlStrEqual(ctxt->context->node->parent->name,
				 BAD_CAST "fake node libxslt"))))
		    return(NULL);
		return(ctxt->context->node->parent);
            case XML_ATTRIBUTE_NODE: {
		xmlAttrPtr att = (xmlAttrPtr) ctxt->context->node;

		return(att->parent);
	    }
            case XML_DOCUMENT_NODE:
            case XML_DOCUMENT_TYPE_NODE:
            case XML_DOCUMENT_FRAG_NODE:
            case XML_HTML_DOCUMENT_NODE:
                return(NULL);
	    case XML_NAMESPACE_DECL: {
		xmlNsPtr ns = (xmlNsPtr) ctxt->context->node;

		if ((ns->next != NULL) &&
		    (ns->next->type != XML_NAMESPACE_DECL))
		    return((xmlNodePtr) ns->next);
                return(NULL);
	    }
	}
    }
    return(NULL);
}

/**
 * xmlXPathNextAncestor:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "ancestor" direction
 * the ancestor axis contains the ancestors of the context node; the ancestors
 * of the context node consist of the parent of context node and the parent's
 * parent and so on; the nodes are ordered in reverse document order; thus the
 * parent is the first node on the axis, and the parent's parent is the second
 * node on the axis
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextAncestor(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    /*
     * the parent of an attribute or namespace node is the element
     * to which the attribute or namespace node is attached
     * !!!!!!!!!!!!!
     */
    if (cur == NULL) {
	if (ctxt->context->node == NULL) return(NULL);
	switch (ctxt->context->node->type) {
            case XML_ELEMENT_NODE:
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
            case XML_ENTITY_REF_NODE:
            case XML_ENTITY_NODE:
            case XML_PI_NODE:
            case XML_COMMENT_NODE:
	    case XML_DTD_NODE:
	    case XML_ELEMENT_DECL:
	    case XML_ATTRIBUTE_DECL:
	    case XML_ENTITY_DECL:
            case XML_NOTATION_NODE:
	    case XML_XINCLUDE_START:
	    case XML_XINCLUDE_END:
		if (ctxt->context->node->parent == NULL)
		    return(NULL);
		if ((ctxt->context->node->parent->type == XML_ELEMENT_NODE) &&
		    ((ctxt->context->node->parent->name[0] == ' ') ||
		     (xmlStrEqual(ctxt->context->node->parent->name,
				 BAD_CAST "fake node libxslt"))))
		    return(NULL);
		return(ctxt->context->node->parent);
            case XML_ATTRIBUTE_NODE: {
		xmlAttrPtr tmp = (xmlAttrPtr) ctxt->context->node;

		return(tmp->parent);
	    }
            case XML_DOCUMENT_NODE:
            case XML_DOCUMENT_TYPE_NODE:
            case XML_DOCUMENT_FRAG_NODE:
            case XML_HTML_DOCUMENT_NODE:
                return(NULL);
	    case XML_NAMESPACE_DECL: {
		xmlNsPtr ns = (xmlNsPtr) ctxt->context->node;

		if ((ns->next != NULL) &&
		    (ns->next->type != XML_NAMESPACE_DECL))
		    return((xmlNodePtr) ns->next);
		/* Bad, how did that namespace end up here ? */
                return(NULL);
	    }
	}
	return(NULL);
    }
    switch (cur->type) {
	case XML_ELEMENT_NODE:
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_ENTITY_NODE:
	case XML_PI_NODE:
	case XML_COMMENT_NODE:
	case XML_NOTATION_NODE:
	case XML_DTD_NODE:
        case XML_ELEMENT_DECL:
        case XML_ATTRIBUTE_DECL:
        case XML_ENTITY_DECL:
	case XML_XINCLUDE_START:
	case XML_XINCLUDE_END:
	    if (cur->parent == NULL)
		return(NULL);
	    if ((cur->parent->type == XML_ELEMENT_NODE) &&
		((cur->parent->name[0] == ' ') ||
		 (xmlStrEqual(cur->parent->name,
			      BAD_CAST "fake node libxslt"))))
		return(NULL);
	    return(cur->parent);
	case XML_ATTRIBUTE_NODE: {
	    xmlAttrPtr att = (xmlAttrPtr) cur;

	    return(att->parent);
	}
	case XML_NAMESPACE_DECL: {
	    xmlNsPtr ns = (xmlNsPtr) cur;

	    if ((ns->next != NULL) &&
	        (ns->next->type != XML_NAMESPACE_DECL))
	        return((xmlNodePtr) ns->next);
	    /* Bad, how did that namespace end up here ? */
            return(NULL);
	}
	case XML_DOCUMENT_NODE:
	case XML_DOCUMENT_TYPE_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_HTML_DOCUMENT_NODE:
	    return(NULL);
    }
    return(NULL);
}

/**
 * xmlXPathNextAncestorOrSelf:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "ancestor-or-self" direction
 * he ancestor-or-self axis contains the context node and ancestors of
 * the context node in reverse document order; thus the context node is
 * the first node on the axis, and the context node's parent the second;
 * parent here is defined the same as with the parent axis.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextAncestorOrSelf(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (cur == NULL)
        return(ctxt->context->node);
    return(xmlXPathNextAncestor(ctxt, cur));
}

/**
 * xmlXPathNextFollowingSibling:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "following-sibling" direction
 * The following-sibling axis contains the following siblings of the context
 * node in document order.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextFollowingSibling(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if ((ctxt->context->node->type == XML_ATTRIBUTE_NODE) ||
	(ctxt->context->node->type == XML_NAMESPACE_DECL))
	return(NULL);
    if (cur == NULL)
        return(ctxt->context->node->next);
    return(cur->next);
}

/**
 * xmlXPathNextPrecedingSibling:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "preceding-sibling" direction
 * The preceding-sibling axis contains the preceding siblings of the context
 * node in reverse document order; the first preceding sibling is first on the
 * axis; the sibling preceding that node is the second on the axis and so on.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextPrecedingSibling(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if ((ctxt->context->node->type == XML_ATTRIBUTE_NODE) ||
	(ctxt->context->node->type == XML_NAMESPACE_DECL))
	return(NULL);
    if (cur == NULL)
        return(ctxt->context->node->prev);
    if ((cur->prev != NULL) && (cur->prev->type == XML_DTD_NODE)) {
	cur = cur->prev;
	if (cur == NULL)
	    return(ctxt->context->node->prev);
    }
    return(cur->prev);
}

/**
 * xmlXPathNextFollowing:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "following" direction
 * The following axis contains all nodes in the same document as the context
 * node that are after the context node in document order, excluding any
 * descendants and excluding attribute nodes and namespace nodes; the nodes
 * are ordered in document order
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextFollowing(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if ((cur != NULL) && (cur->type  != XML_ATTRIBUTE_NODE) &&
        (cur->type != XML_NAMESPACE_DECL) && (cur->children != NULL))
        return(cur->children);

    if (cur == NULL) {
        cur = ctxt->context->node;
        if (cur->type == XML_ATTRIBUTE_NODE) {
            cur = cur->parent;
        } else if (cur->type == XML_NAMESPACE_DECL) {
            xmlNsPtr ns = (xmlNsPtr) cur;

            if ((ns->next == NULL) ||
                (ns->next->type == XML_NAMESPACE_DECL))
                return (NULL);
            cur = (xmlNodePtr) ns->next;
        }
    }
    if (cur == NULL) return(NULL) ; /* ERROR */
    if (cur->next != NULL) return(cur->next) ;
    do {
        cur = cur->parent;
        if (cur == NULL) break;
        if ((cur->type == XML_DOCUMENT_NODE) ||
            (cur->type == XML_HTML_DOCUMENT_NODE))
            return(NULL);
        if (cur->next != NULL) return(cur->next);
    } while (cur != NULL);
    return(cur);
}

/*
 * xmlXPathIsAncestor:
 * @ancestor:  the ancestor node
 * @node:  the current node
 *
 * Check that @ancestor is a @node's ancestor
 *
 * returns 1 if @ancestor is a @node's ancestor, 0 otherwise.
 */
static int
xmlXPathIsAncestor(xmlNodePtr ancestor, xmlNodePtr node) {
    if ((ancestor == NULL) || (node == NULL)) return(0);
    if (node->type == XML_NAMESPACE_DECL)
        return(0);
    if (ancestor->type == XML_NAMESPACE_DECL)
        return(0);
    /* nodes need to be in the same document */
    if (ancestor->doc != node->doc) return(0);
    /* avoid searching if ancestor or node is the root node */
    if (ancestor == (xmlNodePtr) node->doc) return(1);
    if (node == (xmlNodePtr) ancestor->doc) return(0);
    while (node->parent != NULL) {
        if (node->parent == ancestor)
            return(1);
	node = node->parent;
    }
    return(0);
}

/**
 * xmlXPathNextPreceding:
 * @ctxt:  the XPath Parser context
 * @cur:  the current node in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "preceding" direction
 * the preceding axis contains all nodes in the same document as the context
 * node that are before the context node in document order, excluding any
 * ancestors and excluding attribute nodes and namespace nodes; the nodes are
 * ordered in reverse document order
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextPreceding(xmlXPathParserContextPtr ctxt, xmlNodePtr cur)
{
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (cur == NULL) {
        cur = ctxt->context->node;
        if (cur->type == XML_ATTRIBUTE_NODE) {
            cur = cur->parent;
        } else if (cur->type == XML_NAMESPACE_DECL) {
            xmlNsPtr ns = (xmlNsPtr) cur;

            if ((ns->next == NULL) ||
                (ns->next->type == XML_NAMESPACE_DECL))
                return (NULL);
            cur = (xmlNodePtr) ns->next;
        }
    }
    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL))
	return (NULL);
    if ((cur->prev != NULL) && (cur->prev->type == XML_DTD_NODE))
	cur = cur->prev;
    do {
        if (cur->prev != NULL) {
            for (cur = cur->prev; cur->last != NULL; cur = cur->last) ;
            return (cur);
        }

        cur = cur->parent;
        if (cur == NULL)
            return (NULL);
        if ((cur->type == XML_DOCUMENT_NODE) ||
            (cur->type == XML_HTML_DOCUMENT_NODE))
            return(NULL);
    } while (xmlXPathIsAncestor(cur, ctxt->context->node));
    return (cur);
}

/**
 * xmlXPathNextNamespace:
 * @ctxt:  the XPath Parser context
 * @cur:  the current attribute in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "namespace" direction
 * the namespace axis contains the namespace nodes of the context node;
 * the order of nodes on this axis is implementation-defined; the axis will
 * be empty unless the context node is an element
 *
 * We keep the XML namespace node at the end of the list.
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextNamespace(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    xmlXPathContextPtr xpctxt;

    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    xpctxt = ctxt->context;
    if (xpctxt->node->type != XML_ELEMENT_NODE) return(NULL);
    if (cur == NULL) {
        if (xpctxt->tmpNsList != NULL)
	    xmlFree(xpctxt->tmpNsList);
	xpctxt->tmpNsNr = 0;
        if (xmlGetNsListSafe(xpctxt->node->doc, xpctxt->node,
                             &xpctxt->tmpNsList) < 0) {
            xmlXPathErrMemory(xpctxt);
            return(NULL);
        }
        if (xpctxt->tmpNsList != NULL) {
            while (xpctxt->tmpNsList[xpctxt->tmpNsNr] != NULL) {
                xpctxt->tmpNsNr++;
            }
        }
	return((xmlNodePtr) xmlXPathXMLNamespace);
    }
    if (xpctxt->tmpNsNr > 0) {
	return (xmlNodePtr)xpctxt->tmpNsList[--xpctxt->tmpNsNr];
    } else {
	if (xpctxt->tmpNsList != NULL)
	    xmlFree(xpctxt->tmpNsList);
	xpctxt->tmpNsList = NULL;
	return(NULL);
    }
}

/**
 * xmlXPathNextAttribute:
 * @ctxt:  the XPath Parser context
 * @cur:  the current attribute in the traversal
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Traversal function for the "attribute" direction
 * TODO: support DTD inherited default attributes
 *
 * Returns the next element following that axis
 */
xmlNodePtr
xmlXPathNextAttribute(xmlXPathParserContextPtr ctxt, xmlNodePtr cur) {
    if ((ctxt == NULL) || (ctxt->context == NULL)) return(NULL);
    if (ctxt->context->node == NULL)
	return(NULL);
    if (ctxt->context->node->type != XML_ELEMENT_NODE)
	return(NULL);
    if (cur == NULL)
        return((xmlNodePtr)ctxt->context->node->properties);
    return((xmlNodePtr)cur->next);
}

/************************************************************************
 *									*
 *		Implicit tree core function library			*
 *									*
 ************************************************************************/

static xmlDocPtr
xmlXPathGetRoot(xmlXPathContextPtr ctxt) {
    xmlNodePtr node = ctxt->node;

    if (node == NULL)
        return(ctxt->doc);

    if (node->type == XML_NAMESPACE_DECL) {
        xmlNsPtr ns = (xmlNsPtr) node;
        xmlNodePtr parent = (xmlNodePtr) ns->next;

        return(parent->doc);
    } else {
        return(node->doc);
    }
}

/**
 * xmlXPathRoot:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Initialize the context to the root of the document
 */
void
xmlXPathRoot(xmlXPathParserContextPtr ctxt) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr result;
    xmlDocPtr doc;

    if ((ctxt == NULL) || (ctxt->context == NULL))
	return;

    xpctxt = ctxt->context;
    result = xmlXPathCacheNewNodeSet(xpctxt);
    if (result == NULL)
        return;

    doc = xmlXPathGetRoot(xpctxt);
    if (xmlXPathCacheNodeSetAdd(xpctxt, result->nodesetval,
                                (xmlNodePtr) doc) < 0)
        return;

    valuePush(ctxt, result);
}

/************************************************************************
 *									*
 *		The explicit core function library			*
 *http://www.w3.org/Style/XSL/Group/1999/07/xpath-19990705.html#corelib	*
 *									*
 ************************************************************************/


/**
 * xmlXPathLastFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the last() XPath function
 *    number last()
 * The last function returns the number of nodes in the context node list.
 */
void
xmlXPathLastFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;

    CHECK_ARITY(0);
    xpctxt = ctxt->context;
    if (xpctxt->contextSize >= 0) {
	valuePush(ctxt,
	    xmlXPathCacheNewFloat(xpctxt, xpctxt->contextSize));
    } else {
	xmlXPathCErr(xpctxt, XPATH_INVALID_CTXT_SIZE);
    }
}

/**
 * xmlXPathPositionFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the position() XPath function
 *    number position()
 * The position function returns the position of the context node in the
 * context node list. The first position is 1, and so the last position
 * will be equal to last().
 */
void
xmlXPathPositionFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;

    CHECK_ARITY(0);
    xpctxt = ctxt->context;
    if (xpctxt->proximityPosition >= 0) {
	valuePush(ctxt, xmlXPathCacheNewFloat(xpctxt,
                                              xpctxt->proximityPosition));
    } else {
	xmlXPathCErr(xpctxt, XPATH_INVALID_CTXT_POSITION);
    }
}

/**
 * xmlXPathCountFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the count() XPath function
 *    number count(node-set)
 */
void
xmlXPathCountFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr cur;
    int result = 0;

    CHECK_ARITY(1);
    xpctxt = ctxt->context;
    cur = valuePop(ctxt);
    if ((cur == NULL) ||
	((cur->type != XPATH_NODESET) &&
         (cur->type != XPATH_XSLT_TREE)))
	xmlXPathCErr(xpctxt, XPATH_INVALID_TYPE);

    if (cur->nodesetval != NULL)
        result = cur->nodesetval->nodeNr;

    xmlXPathReleaseObject(xpctxt, cur);
    valuePush(ctxt, xmlXPathCacheNewFloat(xpctxt, result));
}

/**
 * xmlXPathGetElementsByIds:
 * @doc:  the document
 * @ids:  a whitespace separated list of IDs
 *
 * Selects elements by their unique ID.
 *
 * Returns a node-set of selected elements.
 */
static int
xmlXPathAddElementsByIds(xmlXPathContextPtr ctxt, xmlNodeSetPtr set,
                         xmlDocPtr doc, const xmlChar *ids) {
    const xmlChar *cur = ids;
    xmlChar *ID;
    xmlAttrPtr attr;
    xmlNodePtr elem = NULL;

    while (IS_BLANK_CH(*cur)) cur++;
    while (*cur != 0) {
	while ((!IS_BLANK_CH(*cur)) && (*cur != 0))
	    cur++;

        ID = xmlStrndup(ids, cur - ids);
	if (ID == NULL) {
            xmlXPathErrMemory(ctxt);
            return(-1);
        }

        /*
         * We used to check the fact that the value passed
         * was an NCName, but this generated much troubles for
         * me and Aleksey Sanin, people blatantly violated that
         * constraint, like Visa3D spec.
         * if (xmlValidateNCName(ID, 1) == 0)
         */
        attr = xmlGetID(doc, ID);
        xmlFree(ID);
        if (attr != NULL) {
            if (attr->type == XML_ATTRIBUTE_NODE)
                elem = attr->parent;
            else if (attr->type == XML_ELEMENT_NODE)
                elem = (xmlNodePtr) attr;
            else
                elem = NULL;
            if (elem != NULL) {
                if (xmlXPathCacheNodeSetAdd(ctxt, set, elem) < 0) {
                    xmlXPathErrMemory(ctxt);
                    return(-1);
                }
            }
        }

	while (IS_BLANK_CH(*cur)) cur++;
	ids = cur;
    }

    return(0);
}

/**
 * xmlXPathIdFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the id() XPath function
 *    node-set id(object)
 * The id function selects elements by their unique ID
 * (see [5.2.1 Unique IDs]). When the argument to id is of type node-set,
 * then the result is the union of the result of applying id to the
 * string value of each of the nodes in the argument node-set. When the
 * argument to id is of any other type, the argument is converted to a
 * string as if by a call to the string function; the string is split
 * into a whitespace-separated list of tokens (whitespace is any sequence
 * of characters matching the production S); the result is a node-set
 * containing the elements in the same document as the context node that
 * have a unique ID equal to any of the tokens in the list.
 */
void
xmlXPathIdFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *tokens = NULL;
    xmlNodeSetPtr set;
    xmlXPathObjectPtr obj, ret;
    xmlDocPtr doc;

    CHECK_ARITY(1);

    xpctxt = ctxt->context;

    obj = valuePop(ctxt);
    if (obj == NULL) {
        xmlXPathCErr(xpctxt, XPATH_INVALID_OPERAND);
        return;
    }

    ret = xmlXPathCacheNewNodeSet(xpctxt);
    if (ret == NULL) {
        xmlXPathErrMemory(xpctxt);
        goto error;
    }
    set = ret->nodesetval;

    doc = xmlXPathGetRoot(xpctxt);

    if ((obj->type == XPATH_NODESET) || (obj->type == XPATH_XSLT_TREE)) {
	int i;

        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            if (tokens != NULL)
                xmlFree(tokens);
            tokens = xmlXPathCastNodeToString(obj->nodesetval->nodeTab[i]);
            if (tokens == NULL) {
                xmlXPathErrMemory(xpctxt);
                goto error;
            }
            if (xmlXPathAddElementsByIds(xpctxt, set, doc, tokens) < 0)
                goto error;
        }
    } else {
        tokens = xmlXPathCastToString(obj);
        if (tokens == NULL) {
            xmlXPathErrMemory(xpctxt);
            goto error;
        }
        if (xmlXPathAddElementsByIds(xpctxt, set, doc, tokens) < 0)
            goto error;
    }

error:
    if (ret != NULL)
        valuePush(ctxt, ret);

    xmlFree(tokens);
    xmlXPathReleaseObject(xpctxt, obj);
}

static xmlNodePtr
xmlXPathGetNodeArg(xmlXPathContextPtr ctxt, int nargs) {
    xmlNodePtr node = NULL;

    if (nargs == 0) {
        node = ctxt->node;
    } else if (nargs == 1) {
        xmlXPathObjectPtr cur;

        cur = xmlXPathValuePopInternal(ctxt);
        if ((cur == NULL) ||
            ((cur->type != XPATH_NODESET) &&
             (cur->type != XPATH_XSLT_TREE))) {
            xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
        } else if ((cur->nodesetval != NULL) &&
                   (cur->nodesetval->nodeNr > 0)) {
            node = cur->nodesetval->nodeTab[0];
        }

        xmlXPathReleaseObject(ctxt, cur);
    } else {
        xmlXPathCErr(ctxt, XPATH_INVALID_ARITY);
    }

    return(node);
}

static const xmlChar *
xmlXPathLocalName(xmlNodePtr node) {
    const xmlChar *name = BAD_CAST "";

    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
            if (node->name[0] != ' ')
                name = node->name;
            break;

        case XML_PI_NODE:
            name = node->name;
            break;

        case XML_NAMESPACE_DECL: {
            xmlNsPtr ns = (xmlNsPtr) node;

            if (ns->prefix != NULL)
                name = ns->prefix;
            break;
        }

        default:
            break;
    }

    return(name);
}

/**
 * xmlXPathLocalNameFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the local-name() XPath function
 *    string local-name(node-set?)
 * The local-name function returns a string containing the local part
 * of the name of the node in the argument node-set that is first in
 * document order. If the node-set is empty or the first node has no
 * name, an empty string is returned. If the argument is omitted it
 * defaults to the context node.
 */
void
xmlXPathLocalNameFunction(xmlXPathParserContextPtr ctxt, int nargs)
{
    xmlXPathContextPtr xpctxt;
    xmlNodePtr node;
    const xmlChar *name = BAD_CAST "";

    if ((ctxt == NULL) || (ctxt->context == NULL))
        return;

    xpctxt = ctxt->context;

    node = xmlXPathGetNodeArg(xpctxt, nargs);
    if (node != NULL)
        name = xmlXPathLocalName(node);

    valuePush(ctxt, xmlXPathCacheNewString(xpctxt, name));
}

static const xmlChar *
xmlXPathNamespaceUri(xmlNodePtr node) {
    if ((node != NULL) &&
        ((node->type == XML_ELEMENT_NODE) ||
         (node->type == XML_ATTRIBUTE_NODE)) &&
        (node->ns != NULL))
        return(node->ns->href);
    else
        return BAD_CAST "";
}

/**
 * xmlXPathNamespaceURIFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the namespace-uri() XPath function
 *    string namespace-uri(node-set?)
 * The namespace-uri function returns a string containing the
 * namespace URI of the expanded name of the node in the argument
 * node-set that is first in document order. If the node-set is empty,
 * the first node has no name, or the expanded name has no namespace
 * URI, an empty string is returned. If the argument is omitted it
 * defaults to the context node.
 */
void
xmlXPathNamespaceURIFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlNodePtr node;
    const xmlChar *uri = BAD_CAST "";

    if ((ctxt == NULL) || (ctxt->context == NULL))
        return;

    xpctxt = ctxt->context;

    node = xmlXPathGetNodeArg(xpctxt, nargs);
    if (node != NULL)
        uri = xmlXPathNamespaceUri(node);

    valuePush(ctxt, xmlXPathCacheNewString(xpctxt, uri));
}

static int
xmlXPathName(xmlXPathContextPtr ctxt, xmlXPathItem *result,
             xmlNodePtr node) {
    xmlChar *name = BAD_CAST "";
    int isCopy = 1;

    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
            if (node->name[0] == ' ')
                break;

            if ((node->ns == NULL) || (node->ns->prefix == NULL)) {
                name = (xmlChar *) node->name;
            } else {
                name = xmlBuildQName(node->name, node->ns->prefix,
                                     NULL, 0);
                if (name == NULL) {
                    xmlXPathErrMemory(ctxt);
                    return(-1);
                }

                isCopy = 0;
            }
            break;

        case XML_PI_NODE:
            name = (xmlChar *) node->name;
            break;

        case XML_NAMESPACE_DECL: {
            xmlNsPtr ns = (xmlNsPtr) node;

            if (ns->prefix != NULL)
                name = (xmlChar *) ns->prefix;
            break;
        }

        default:
            break;
    }

    result->type = XPATH_STRING;
    result->isCopy = isCopy;
    result->as.string = name;

    return(0);
}

/**
 * xmlXPathStringFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the string() XPath function
 *    string string(object?)
 * The string function converts an object to a string as follows:
 *    - A node-set is converted to a string by returning the value of
 *      the node in the node-set that is first in document order.
 *      If the node-set is empty, an empty string is returned.
 *    - A number is converted to a string as follows
 *      + NaN is converted to the string NaN
 *      + positive zero is converted to the string 0
 *      + negative zero is converted to the string 0
 *      + positive infinity is converted to the string Infinity
 *      + negative infinity is converted to the string -Infinity
 *      + if the number is an integer, the number is represented in
 *        decimal form as a Number with no decimal point and no leading
 *        zeros, preceded by a minus sign (-) if the number is negative
 *      + otherwise, the number is represented in decimal form as a
 *        Number including a decimal point with at least one digit
 *        before the decimal point and at least one digit after the
 *        decimal point, preceded by a minus sign (-) if the number
 *        is negative; there must be no leading zeros before the decimal
 *        point apart possibly from the one required digit immediately
 *        before the decimal point; beyond the one required digit
 *        after the decimal point there must be as many, but only as
 *        many, more digits as are needed to uniquely distinguish the
 *        number from all other IEEE 754 numeric values.
 *    - The boolean false value is converted to the string false.
 *      The boolean true value is converted to the string true.
 *
 * If the argument is omitted, it defaults to a node-set with the
 * context node as its only member.
 */
void
xmlXPathStringFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *content;

    if (ctxt == NULL)
        return;

    xpctxt = ctxt->context;

    if (nargs == 0) {
        if (xpctxt == NULL)
            return;
        content = xmlNodeGetContent(xpctxt->node);
    } else {
        xmlXPathObjectPtr cur;

        if ((ctxt->value != NULL) && (ctxt->value->type == XPATH_STRING))
            return;
        cur = valuePop(ctxt);
        content = xmlXPathCastToString(cur);
        xmlXPathReleaseObject(xpctxt, cur);
    }

    if (content == NULL)
        xmlXPathErrMemory(xpctxt);

    valuePush(ctxt, xmlXPathCacheWrapString(xpctxt, content));
}

/**
 * xmlXPathStringLengthFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the string-length() XPath function
 *    number string-length(string?)
 * The string-length returns the number of characters in the string
 * (see [3.6 Strings]). If the argument is omitted, it defaults to
 * the context node converted to a string, in other words the value
 * of the context node.
 */
void
xmlXPathStringLengthFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    int len = 0;

    if ((ctxt == NULL) || (ctxt->context == NULL))
        return;

    xpctxt = ctxt->context;

    if (nargs == 0) {
        xmlChar *content;

        content = xmlNodeGetContent(xpctxt->node);
        if (content == NULL)
            xmlXPathErrMemory(xpctxt);
        len = xmlUTF8Strlen(content);
        xmlFree(content);
    } else {
        xmlXPathObjectPtr cur;

        cur = valuePop(ctxt);
        if (cur == NULL) {
            xmlXPathCErr(xpctxt, XPATH_STACK_ERROR);
            return;
        }

        if (cur->type == XPATH_STRING) {
            len = xmlUTF8Strlen(cur->stringval);
        } else {
            xmlChar *content = xmlXPathCastToString(cur);

            if (content == NULL)
                xmlXPathErrMemory(xpctxt);
            len = xmlUTF8Strlen(content);
            xmlFree(content);
        }

        xmlXPathReleaseObject(xpctxt, cur);
    }

    valuePush(ctxt, xmlXPathCacheNewFloat(xpctxt, len));
}

typedef struct {
    xmlChar *string;
    int len;
} xmlConcatRec;

/**
 * xmlXPathConcatFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the concat() XPath function
 *    string concat(string, string, string*)
 * The concat function returns the concatenation of its arguments.
 */
void
xmlXPathConcatFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlConcatRec *recs;
    xmlChar *res = NULL;
    int totalSize = 0;
    int i, j;

    if (ctxt == NULL) return;
    if (nargs < 2) {
	CHECK_ARITY(2);
    }

    xpctxt = ctxt->context;

    recs = xmlMalloc(nargs * sizeof(recs[0]));
    if (recs == NULL) {
        xmlXPathErrMemory(xpctxt);
        return;
    }
    memset(recs, 0, nargs * sizeof(recs[0]));

    for (i = 0; i < nargs; i++) {
        xmlXPathObjectPtr obj = ctxt->valueTab[ctxt->valueNr - nargs + i];
        xmlChar *string;
        size_t len;

        if (obj == NULL)
            string = NULL;
        else if (obj->type == XPATH_STRING)
            string = obj->stringval;
        else
            string = xmlXPathCastToString(obj);

        if (string == NULL) {
            recs[i].string = NULL;
            recs[i].len = 0;
        } else {
            recs[i].string = string;

            len = strlen((char *) string);
            if (len > (size_t) XML_MAX_ITEMS - totalSize) {
                xmlXPathErrMemory(xpctxt);
                goto error;
            }
            recs[i].len = len;

            totalSize += len;
        }
    }

    res = xmlMalloc(totalSize + 1);
    if (res == NULL) {
        xmlXPathErrMemory(xpctxt);
        goto error;
    }

    for (i = 0, j = 0; i < nargs; i++) {
        xmlConcatRec *rec = &recs[i];

        if (rec->string != NULL) {
            memcpy(res + j, rec->string, rec->len);
            j += rec->len;
        }
    }

    res[j] = 0;

error:
    for (i = nargs - 1; i >= 0; i--) {
        xmlXPathObjectPtr obj = valuePop(ctxt);

        if ((obj != NULL) && (obj->type != XPATH_STRING))
            xmlFree(recs[i].string);

        xmlXPathReleaseObject(xpctxt, obj);
    }

    if (res != NULL)
        valuePush(ctxt, xmlXPathCacheWrapString(xpctxt, res));

    xmlFree(recs);
}

/**
 * xmlXPathContainsFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the contains() XPath function
 *    boolean contains(string, string)
 * The contains function returns true if the first argument string
 * contains the second argument string, and otherwise returns false.
 */
void
xmlXPathContainsFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlChar *hay, *needle;
    int result;

    CHECK_ARITY(2);
    needle = xmlXPathPopString(ctxt);
    hay = xmlXPathPopString(ctxt);
    if (ctxt->error)
        goto error;

    result = (strstr((char *) hay, (char *) needle) != NULL);
    valuePush(ctxt, xmlXPathCacheNewBoolean(ctxt->context, result));

error:
    xmlFree(hay);
    xmlFree(needle);
}

/**
 * xmlXPathStartsWithFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the starts-with() XPath function
 *    boolean starts-with(string, string)
 * The starts-with function returns true if the first argument string
 * starts with the second argument string, and otherwise returns false.
 */
void
xmlXPathStartsWithFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlChar *hay, *needle;
    size_t len;
    int result;

    CHECK_ARITY(2);
    needle = xmlXPathPopString(ctxt);
    hay = xmlXPathPopString(ctxt);
    if (ctxt->error)
        goto error;

    len = strlen((char *) needle);
    result = (strncmp((char *) hay, (char *) needle, len) == 0);

    valuePush(ctxt, xmlXPathCacheNewBoolean(ctxt->context, result));

error:
    xmlFree(hay);
    xmlFree(needle);
}

/**
 * xmlXPathSubstringFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the substring() XPath function
 *    string substring(string, number, number?)
 * The substring function returns the substring of the first argument
 * starting at the position specified in the second argument with
 * length specified in the third argument. For example,
 * substring("12345",2,3) returns "234". If the third argument is not
 * specified, it returns the substring starting at the position specified
 * in the second argument and continuing to the end of the string. For
 * example, substring("12345",2) returns "2345".  More precisely, each
 * character in the string (see [3.6 Strings]) is considered to have a
 * numeric position: the position of the first character is 1, the position
 * of the second character is 2 and so on. The returned substring contains
 * those characters for which the position of the character is greater than
 * or equal to the second argument and, if the third argument is specified,
 * less than the sum of the second and third arguments; the comparisons
 * and addition used for the above follow the standard IEEE 754 rules. Thus:
 *  - substring("12345", 1.5, 2.6) returns "234"
 *  - substring("12345", 0, 3) returns "12"
 *  - substring("12345", 0 div 0, 3) returns ""
 *  - substring("12345", 1, 0 div 0) returns ""
 *  - substring("12345", -42, 1 div 0) returns "12345"
 *  - substring("12345", -1 div 0, 1 div 0) returns ""
 */
void
xmlXPathSubstringFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *str;
    double le=0, in;
    int i = 1, j = INT_MAX;

    if (ctxt == NULL)
        return;

    xpctxt = ctxt->context;

    if ((nargs < 2) || (nargs > 3)) {
        xmlXPathCErr(xpctxt, XPATH_INVALID_ARITY);
        return;
    }

    /*
     * take care of possible last (position) argument
    */
    if (nargs == 3)
	le = xmlXPathPopNumber(ctxt);

    in = xmlXPathPopNumber(ctxt);
    str = xmlXPathPopString(ctxt);
    if (ctxt->error)
        return;

    if (!(in < INT_MAX)) { /* Logical NOT to handle NaNs */
        i = INT_MAX;
    } else if (in >= 1.0) {
        i = (int)in;
        if (in - floor(in) >= 0.5)
            i += 1;
    }

    if (nargs == 3) {
        double rin, rle, end;

        rin = floor(in);
        if (in - rin >= 0.5)
            rin += 1.0;

        rle = floor(le);
        if (le - rle >= 0.5)
            rle += 1.0;

        end = rin + rle;
        if (!(end >= 1.0)) { /* Logical NOT to handle NaNs */
            j = 1;
        } else if (end < INT_MAX) {
            j = (int)end;
        }
    }

    i -= 1;
    j -= 1;

    if ((i < j) && (i < xmlUTF8Strlen(str))) {
        xmlChar *ret = xmlUTF8Strsub(str, i, j - i);
        if (ret == NULL)
            xmlXPathErrMemory(xpctxt);
	valuePush(ctxt, xmlXPathCacheNewString(xpctxt, ret));
	xmlFree(ret);
    } else {
	valuePush(ctxt, xmlXPathCacheNewCString(xpctxt, ""));
    }

    xmlFree(str);
}

/**
 * xmlXPathSubstringBeforeFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the substring-before() XPath function
 *    string substring-before(string, string)
 * The substring-before function returns the substring of the first
 * argument string that precedes the first occurrence of the second
 * argument string in the first argument string, or the empty string
 * if the first argument string does not contain the second argument
 * string. For example, substring-before("1999/04/01","/") returns 1999.
 */
void
xmlXPathSubstringBeforeFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *str, *find;
    const xmlChar *point;
    xmlChar *result;

    CHECK_ARITY(2);

    xpctxt = ctxt->context;

    find = xmlXPathPopString(ctxt);
    str = xmlXPathPopString(ctxt);
    if (ctxt->error != 0)
        goto error;

    point = BAD_CAST strstr((char *) str, (char *) find);
    if (point == NULL) {
        result = xmlStrdup(BAD_CAST "");
    } else {
        size_t len = point - str;

        if (len > INT_MAX) {
            xmlXPathErrMemory(xpctxt);
            goto error;
        }
        result = xmlStrndup(str, len);
    }
    if (result == NULL) {
        xmlXPathErrMemory(xpctxt);
        goto error;
    }
    valuePush(ctxt, xmlXPathCacheWrapString(xpctxt, result));

error:
    xmlFree(str);
    xmlFree(find);
}

/**
 * xmlXPathSubstringAfterFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the substring-after() XPath function
 *    string substring-after(string, string)
 * The substring-after function returns the substring of the first
 * argument string that follows the first occurrence of the second
 * argument string in the first argument string, or the empty string
 * if the first argument string does not contain the second argument
 * string. For example, substring-after("1999/04/01","/") returns 04/01,
 * and substring-after("1999/04/01","19") returns 99/04/01.
 */
void
xmlXPathSubstringAfterFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *str, *find;
    const xmlChar *point;
    xmlChar *result;

    CHECK_ARITY(2);

    xpctxt = ctxt->context;

    find = xmlXPathPopString(ctxt);
    str = xmlXPathPopString(ctxt);
    if (ctxt->error != 0)
        goto error;

    point = BAD_CAST strstr((char *) str, (char *) find);
    if (point == NULL) {
        result = xmlStrdup(BAD_CAST "");
    } else {
        size_t len = strlen((char *) find);

        result = xmlStrdup(point + len);
    }
    if (result == NULL) {
        xmlXPathErrMemory(xpctxt);
        goto error;
    }
    valuePush(ctxt, xmlXPathCacheWrapString(xpctxt, result));

error:
    xmlFree(str);
    xmlFree(find);
}

/**
 * xmlXPathNormalizeFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the normalize-space() XPath function
 *    string normalize-space(string?)
 * The normalize-space function returns the argument string with white
 * space normalized by stripping leading and trailing whitespace
 * and replacing sequences of whitespace characters by a single
 * space. Whitespace characters are the same allowed by the S production
 * in XML. If the argument is omitted, it defaults to the context
 * node converted to a string, in other words the value of the context node.
 */
void
xmlXPathNormalizeFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *str, *source, *target;
    int blank;

    if (ctxt == NULL) return;

    xpctxt = ctxt->context;

    if (nargs == 0) {
        /* Use current context node */
        str = xmlNodeGetContent(xpctxt->node);
        if (str == NULL) {
            xmlXPathErrMemory(xpctxt);
            return;
        }
    } else {
        CHECK_ARITY(1);
        str = xmlXPathPopString(ctxt);
        if (ctxt->error)
            return;
    }

    source = str;
    target = str;

    /* Skip leading whitespaces */
    while (IS_BLANK_CH(*source))
        source++;

    /* Collapse intermediate whitespaces, and skip trailing whitespaces */
    blank = 0;
    while (*source) {
        if (IS_BLANK_CH(*source)) {
	    blank = 1;
        } else {
            if (blank) {
                *target++ = 0x20;
                blank = 0;
            }
            *target++ = *source;
        }
        source++;
    }
    *target = 0;

    valuePush(ctxt, xmlXPathCacheWrapString(xpctxt, str));
}

/**
 * xmlXPathTranslateFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the translate() XPath function
 *    string translate(string, string, string)
 * The translate function returns the first argument string with
 * occurrences of characters in the second argument string replaced
 * by the character at the corresponding position in the third argument
 * string. For example, translate("bar","abc","ABC") returns the string
 * BAr. If there is a character in the second argument string with no
 * character at a corresponding position in the third argument string
 * (because the second argument string is longer than the third argument
 * string), then occurrences of that character in the first argument
 * string are removed. For example, translate("--aaa--","abc-","ABC")
 * returns "AAA". If a character occurs more than once in second
 * argument string, then the first occurrence determines the replacement
 * character. If the third argument string is longer than the second
 * argument string, then excess characters are ignored.
 */
void
xmlXPathTranslateFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlChar *str, *from, *to;
    xmlBufPtr target;
    int offset, max;
    int ch;
    const xmlChar *point;
    xmlChar *cptr, *content;

    CHECK_ARITY(3);

    xpctxt = ctxt->context;

    to = xmlXPathPopString(ctxt);
    from = xmlXPathPopString(ctxt);
    str = xmlXPathPopString(ctxt);
    if (ctxt->error != 0)
        goto error;

    /*
     * Account for quadratic runtime
     */
    if (xpctxt->opLimit != 0) {
        unsigned long f1 = xmlStrlen(from);
        unsigned long f2 = xmlStrlen(str);

        if ((f1 > 0) && (f2 > 0)) {
            unsigned long p;

            f1 = f1 / 10 + 1;
            f2 = f2 / 10 + 1;
            p = f1 > ULONG_MAX / f2 ? ULONG_MAX : f1 * f2;
            if (xmlXPathCheckOpLimit(xpctxt, p) < 0)
                goto error;
        }
    }

    target = xmlBufCreate(50);
    if (target == NULL) {
        xmlXPathErrMemory(xpctxt);
        goto error;
    }

    max = xmlUTF8Strlen(to);
    for (cptr = str; (ch=*cptr); ) {
        offset = xmlUTF8Strloc(from, cptr);
        if (offset >= 0) {
            if (offset < max) {
                point = xmlUTF8Strpos(to, offset);
                if (point)
                    xmlBufAdd(target, point, xmlUTF8Strsize(point, 1));
            }
        } else
            xmlBufAdd(target, cptr, xmlUTF8Strsize(cptr, 1));

        /* Step to next character in input */
        cptr++;
        if ( ch & 0x80 ) {
            /* if not simple ascii, verify proper format */
            if ( (ch & 0xc0) != 0xc0 ) {
                xmlXPathCErr(xpctxt, XPATH_INVALID_CHAR_ERROR);
                break;
            }
            /* then skip over remaining bytes for this char */
            while ( (ch <<= 1) & 0x80 )
                if ( (*cptr++ & 0xc0) != 0x80 ) {
                    xmlXPathCErr(xpctxt, XPATH_INVALID_CHAR_ERROR);
                    break;
                }
            if (ch & 0x80) /* must have had error encountered */
                break;
        }
    }

    content = xmlBufDetach(target);
    if (content == NULL)
        xmlXPathErrMemory(xpctxt);
    else
        valuePush(ctxt, xmlXPathCacheWrapString(xpctxt, content));
    xmlBufFree(target);

error:
    xmlFree(str);
    xmlFree(from);
    xmlFree(to);
}

/**
 * xmlXPathBooleanFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the boolean() XPath function
 *    boolean boolean(object)
 * The boolean function converts its argument to a boolean as follows:
 *    - a number is true if and only if it is neither positive or
 *      negative zero nor NaN
 *    - a node-set is true if and only if it is non-empty
 *    - a string is true if and only if its length is non-zero
 */
void
xmlXPathBooleanFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(1);
    if (ctxt->value == NULL) {
        xmlXPathCErr(ctxt->context, XPATH_STACK_ERROR);
        return;
    }
    if (ctxt->value->type != XPATH_BOOLEAN)
        xmlXPathBooleanFuncInternal(ctxt);
}

/**
 * xmlXPathNotFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the not() XPath function
 *    boolean not(boolean)
 * The not function returns true if its argument is false,
 * and false otherwise.
 */
void
xmlXPathNotFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(1);
    if (ctxt->value == NULL) {
        xmlXPathCErr(ctxt->context, XPATH_STACK_ERROR);
        return;
    }
    if (ctxt->value->type != XPATH_BOOLEAN) {
        xmlXPathBooleanFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }
    if (ctxt->value != NULL)
        ctxt->value->boolval = !ctxt->value->boolval;
}

/**
 * xmlXPathTrueFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the true() XPath function
 *    boolean true()
 */
void
xmlXPathTrueFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(0);
    valuePush(ctxt, xmlXPathCacheNewBoolean(ctxt->context, 1));
}

/**
 * xmlXPathFalseFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the false() XPath function
 *    boolean false()
 */
void
xmlXPathFalseFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(0);
    valuePush(ctxt, xmlXPathCacheNewBoolean(ctxt->context, 0));
}

/**
 * xmlXPathLangFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the lang() XPath function
 *    boolean lang(string)
 * The lang function returns true or false depending on whether the
 * language of the context node as specified by xml:lang attributes
 * is the same as or is a sublanguage of the language specified by
 * the argument string. The language of the context node is determined
 * by the value of the xml:lang attribute on the context node, or, if
 * the context node has no xml:lang attribute, by the value of the
 * xml:lang attribute on the nearest ancestor of the context node that
 * has an xml:lang attribute. If there is no such attribute, then lang
 * returns false. If there is such an attribute, then lang returns
 * true if the attribute value is equal to the argument ignoring case,
 * or if there is some suffix starting with - such that the attribute
 * value is equal to the argument ignoring that suffix of the attribute
 * value and ignoring case.
 */
void
xmlXPathLangFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlNodePtr cur;
    xmlChar *theLang;
    xmlChar *lang;
    int ret = 0;
    int i;

    CHECK_ARITY(1);
    lang = xmlXPathPopString(ctxt);
    if (ctxt->error)
        return;

    xpctxt = ctxt->context;
    cur = xpctxt->node;
    while (cur != NULL) {
        if (xmlNodeGetAttrValue(cur, BAD_CAST "lang", XML_XML_NAMESPACE,
                                &theLang) < 0)
            xmlXPathErrMemory(xpctxt);
        if (theLang != NULL)
            break;
        cur = cur->parent;
    }
    if ((theLang != NULL) && (lang != NULL)) {
        for (i = 0;lang[i] != 0;i++)
            if (toupper(lang[i]) != toupper(theLang[i]))
                goto not_equal;
        if ((theLang[i] == 0) || (theLang[i] == '-'))
            ret = 1;
    }

not_equal:
    valuePush(ctxt, xmlXPathCacheNewBoolean(xpctxt, ret));

    xmlFree(theLang);
    xmlFree(lang);
}

/**
 * xmlXPathNumberFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the number() XPath function
 *    number number(object?)
 */
void
xmlXPathNumberFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    double res;

    if (ctxt == NULL) return;

    xpctxt = ctxt->context;

    if (nargs == 0) {
	if (xpctxt->node == NULL) {
	    valuePush(ctxt, xmlXPathCacheNewFloat(xpctxt, 0.0));
	} else {
	    xmlChar* content = xmlNodeGetContent(xpctxt->node);
            if (content == NULL)
                xmlXPathErrMemory(xpctxt);

	    res = xmlXPathStringEvalNumber(content);
	    valuePush(ctxt, xmlXPathCacheNewFloat(xpctxt, res));
	    xmlFree(content);
	}
	return;
    }

    CHECK_ARITY(1);
    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER))
        xmlXPathNumberFuncInternal(ctxt);
}

/**
 * xmlXPathSumFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the sum() XPath function
 *    number sum(node-set)
 * The sum function returns the sum of the values of the nodes in
 * the argument node-set.
 */
void
xmlXPathSumFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlXPathContextPtr xpctxt;
    xmlXPathObjectPtr cur;
    int i;
    double res = 0.0;

    CHECK_ARITY(1);

    xpctxt = ctxt->context;

    if ((ctxt->value == NULL) ||
	((ctxt->value->type != XPATH_NODESET) &&
	 (ctxt->value->type != XPATH_XSLT_TREE))) {
	xmlXPathCErr(xpctxt, XPATH_INVALID_TYPE);
        return;
    }
    cur = valuePop(ctxt);

    if ((cur->nodesetval != NULL) && (cur->nodesetval->nodeNr != 0)) {
	for (i = 0; i < cur->nodesetval->nodeNr; i++) {
	    res += xmlXPathNodeToNumberInternal(xpctxt,
                                                cur->nodesetval->nodeTab[i]);
	}
    }
    xmlXPathReleaseObject(xpctxt, cur);
    valuePush(ctxt, xmlXPathCacheNewFloat(xpctxt, res));
}

/**
 * xmlXPathFloorFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the floor() XPath function
 *    number floor(number)
 * The floor function returns the largest (closest to positive infinity)
 * number that is not greater than the argument and that is an integer.
 */
void
xmlXPathFloorFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(1);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval = floor(ctxt->value->floatval);
}

/**
 * xmlXPathCeilingFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the ceiling() XPath function
 *    number ceiling(number)
 * The ceiling function returns the smallest (closest to negative infinity)
 * number that is not less than the argument and that is an integer.
 */
void
xmlXPathCeilingFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(1);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

#ifdef _AIX
    /* Work around buggy ceil() function on AIX */
    ctxt->value->floatval = copysign(ceil(ctxt->value->floatval), ctxt->value->floatval);
#else
    ctxt->value->floatval = ceil(ctxt->value->floatval);
#endif
}

/**
 * xmlXPathRoundFunction:
 * @ctxt:  the XPath Parser context
 * @nargs:  the number of arguments
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Implement the round() XPath function
 *    number round(number)
 * The round function returns the number that is closest to the
 * argument and that is an integer. If there are two such numbers,
 * then the one that is closest to positive infinity is returned.
 */
void
xmlXPathRoundFunction(xmlXPathParserContextPtr ctxt, int nargs) {
    CHECK_ARITY(1);

    if ((ctxt->value == NULL) || (ctxt->value->type != XPATH_NUMBER)) {
        xmlXPathNumberFuncInternal(ctxt);
        if (ctxt->error)
            return;
    }

    ctxt->value->floatval = xmlXPathRound(ctxt->value->floatval);
}

/************************************************************************
 *									*
 *			The Parser					*
 *									*
 ************************************************************************/

/*
 * a few forward declarations since we use a recursive call based
 * implementation.
 */
static int xmlXPathCompileExpr(xmlXPathContextPtr ctxt);
static int xmlXPathCompPredicate(xmlXPathContextPtr ctxt, int argIndex,
                                 int filter);
static int xmlXPathCompLocationPath(xmlXPathContextPtr ctxt, int opIndex);
static int xmlXPathCompRelativeLocationPath(xmlXPathContextPtr ctxt,
                                            int opIndex);

/**
 * xmlXPathCurrentChar:
 * @ctxt:  the XPath parser context
 * @cur:  pointer to the beginning of the char
 * @len:  pointer to the length of the char read
 *
 * The current char value, if using UTF-8 this may actually span multiple
 * bytes in the input buffer.
 *
 * Returns the current char value and its length
 */

static int
xmlXPathCurrentChar(xmlXPathContextPtr ctxt, int *len) {
    unsigned char c;
    unsigned int val;
    const xmlChar *cur;

    if (ctxt == NULL)
	return(0);
    cur = ctxt->pctxt.cur;

    /*
     * We are supposed to handle UTF8, check it's valid
     * From rfc2044: encoding of the Unicode values on UTF-8:
     *
     * UCS-4 range (hex.)           UTF-8 octet sequence (binary)
     * 0000 0000-0000 007F   0xxxxxxx
     * 0000 0080-0000 07FF   110xxxxx 10xxxxxx
     * 0000 0800-0000 FFFF   1110xxxx 10xxxxxx 10xxxxxx
     *
     * Check for the 0x110000 limit too
     */
    c = *cur;
    if (c & 0x80) {
	if ((cur[1] & 0xc0) != 0x80)
	    goto encoding_error;
	if ((c & 0xe0) == 0xe0) {

	    if ((cur[2] & 0xc0) != 0x80)
		goto encoding_error;
	    if ((c & 0xf0) == 0xf0) {
		if (((c & 0xf8) != 0xf0) ||
		    ((cur[3] & 0xc0) != 0x80))
		    goto encoding_error;
		/* 4-byte code */
		*len = 4;
		val = (cur[0] & 0x7) << 18;
		val |= (cur[1] & 0x3f) << 12;
		val |= (cur[2] & 0x3f) << 6;
		val |= cur[3] & 0x3f;
	    } else {
	      /* 3-byte code */
		*len = 3;
		val = (cur[0] & 0xf) << 12;
		val |= (cur[1] & 0x3f) << 6;
		val |= cur[2] & 0x3f;
	    }
	} else {
	  /* 2-byte code */
	    *len = 2;
	    val = (cur[0] & 0x1f) << 6;
	    val |= cur[1] & 0x3f;
	}
	if (!IS_CHAR(val)) {
	    xmlXPathCErr(ctxt, XPATH_INVALID_CHAR_ERROR);
            return(0);
	}
	return(val);
    } else {
	/* 1-byte code */
	*len = 1;
	return(*cur);
    }
encoding_error:
    /*
     * If we detect an UTF8 error that probably means that the
     * input encoding didn't get properly advertised in the
     * declaration header. Report the error and switch the encoding
     * to ISO-Latin-1 (if you don't like this policy, just declare the
     * encoding !)
     */
    *len = 0;
    xmlXPathCErr(ctxt, XPATH_ENCODING_ERROR);
    return(0);
}

static xmlChar *
xmlXPathParseNameInternal(xmlXPathContextPtr ctxt, int exclude) {
    const xmlChar *start = ctxt->pctxt.cur;
    xmlChar *ret;
    size_t size;

    size = xmlScanXmlName(start, SIZE_MAX, exclude);
    if (size == 0)
        return(NULL);
    if (size > XML_MAX_NAME_LENGTH) {
        xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
        return(NULL);
    }

    ctxt->pctxt.cur += size;

    ret = xmlStrndup(start, size);
    if (ret == NULL)
        xmlXPathErrMemory(ctxt);

    return(ret);
}

/**
 * xmlXPathParseNCName:
 * @ctxt:  the XPath Parser context
 *
 * parse an XML namespace non qualified name.
 *
 * [NS 3] NCName ::= (Letter | '_') (NCNameChar)*
 *
 * [NS 4] NCNameChar ::= Letter | Digit | '.' | '-' | '_' |
 *                       CombiningChar | Extender
 *
 * Returns the namespace name or NULL
 */

xmlChar *
xmlXPathParseNCName(xmlXPathParserContextPtr ctxt) {
    if ((ctxt == NULL) || (ctxt->cur == NULL))
        return(NULL);

    return(xmlXPathParseNameInternal(ctxt->context, ':'));
}

/**
 * xmlXPathParseQName:
 * @ctxt:  the XPath Parser context
 * @prefix:  a xmlChar **
 *
 * parse an XML qualified name
 *
 * [NS 5] QName ::= (Prefix ':')? LocalPart
 *
 * [NS 6] Prefix ::= NCName
 *
 * [NS 7] LocalPart ::= NCName
 *
 * Returns the function returns the local part, and prefix is updated
 *   to get the Prefix if any.
 */

static xmlChar *
xmlXPathParseQName(xmlXPathContextPtr ctxt, xmlChar **prefix) {
    xmlChar *ret = NULL;

    *prefix = NULL;
    ret = xmlXPathParseNameInternal(ctxt, ':');
    if (ret && CUR == ':') {
        *prefix = ret;
	NEXT;
	ret = xmlXPathParseNameInternal(ctxt, ':');
    }
    return(ret);
}

/**
 * xmlXPathParseName:
 * @ctxt:  the XPath Parser context
 *
 * parse an XML name
 *
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' |
 *                  CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * Returns the namespace name or NULL
 */

xmlChar *
xmlXPathParseName(xmlXPathParserContextPtr ctxt) {
    if ((ctxt == NULL) || (ctxt->cur == NULL))
        return(NULL);

    return(xmlXPathParseNameInternal(ctxt->context, 0));
}

#define MAX_FRAC 20

/**
 * xmlXPathStringEvalNumber:
 * @str:  A string to scan
 *
 *  [30a]  Float  ::= Number ('e' Digits?)?
 *
 *  [30]   Number ::=   Digits ('.' Digits?)?
 *                    | '.' Digits
 *  [31]   Digits ::=   [0-9]+
 *
 * Compile a Number in the string
 * In complement of the Number expression, this function also handles
 * negative values : '-' Number.
 *
 * Returns the double value.
 */
double
xmlXPathStringEvalNumber(const xmlChar *str) {
    const xmlChar *cur = str;
    double ret;
    int ok = 0;
    int isneg = 0;
    int exponent = 0;
    int is_exponent_negative = 0;
#ifdef __GNUC__
    unsigned long tmp = 0;
    double temp;
#endif
    if (cur == NULL) return(0);
    while (IS_BLANK_CH(*cur)) cur++;
    if (*cur == '-') {
	isneg = 1;
	cur++;
    }
    if ((*cur != '.') && ((*cur < '0') || (*cur > '9'))) {
        return(xmlXPathNAN);
    }

#ifdef __GNUC__
    /*
     * tmp/temp is a workaround against a gcc compiler bug
     * http://veillard.com/gcc.bug
     */
    ret = 0;
    while ((*cur >= '0') && (*cur <= '9')) {
	ret = ret * 10;
	tmp = (*cur - '0');
	ok = 1;
	cur++;
	temp = (double) tmp;
	ret = ret + temp;
    }
#else
    ret = 0;
    while ((*cur >= '0') && (*cur <= '9')) {
	ret = ret * 10 + (*cur - '0');
	ok = 1;
	cur++;
    }
#endif

    if (*cur == '.') {
	int v, frac = 0, max;
	double fraction = 0;

        cur++;
	if (((*cur < '0') || (*cur > '9')) && (!ok)) {
	    return(xmlXPathNAN);
	}
        while (*cur == '0') {
	    frac = frac + 1;
	    cur++;
        }
        max = frac + MAX_FRAC;
	while (((*cur >= '0') && (*cur <= '9')) && (frac < max)) {
	    v = (*cur - '0');
	    fraction = fraction * 10 + v;
	    frac = frac + 1;
	    cur++;
	}
	fraction /= pow(10.0, frac);
	ret = ret + fraction;
	while ((*cur >= '0') && (*cur <= '9'))
	    cur++;
    }
    if ((*cur == 'e') || (*cur == 'E')) {
      cur++;
      if (*cur == '-') {
	is_exponent_negative = 1;
	cur++;
      } else if (*cur == '+') {
        cur++;
      }
      while ((*cur >= '0') && (*cur <= '9')) {
        if (exponent < 1000000)
	  exponent = exponent * 10 + (*cur - '0');
	cur++;
      }
    }
    while (IS_BLANK_CH(*cur)) cur++;
    if (*cur != 0) return(xmlXPathNAN);
    if (isneg) ret = -ret;
    if (is_exponent_negative) exponent = -exponent;
    ret *= pow(10.0, (double)exponent);
    return(ret);
}

/**
 * xmlXPathCompNumber:
 * @ctxt:  the XPath Parser context
 *
 *  [30]   Number ::=   Digits ('.' Digits?)?
 *                    | '.' Digits
 *  [31]   Digits ::=   [0-9]+
 *
 * Compile a Number, then push it on the stack
 *
 */
static int
xmlXPathCompNumber(xmlXPathContextPtr ctxt)
{
    xmlXPathOpPtr op;
    double ret = 0.0;
    int ok = 0;
    int exponent = 0;
    int is_exponent_negative = 0;
    int opIndex;
#ifdef __GNUC__
    unsigned long tmp = 0;
    double temp;
#endif

    if ((CUR != '.') && ((CUR < '0') || (CUR > '9'))) {
        xmlXPathCErr(ctxt, XPATH_NUMBER_ERROR);
        return(-1);
    }
#ifdef __GNUC__
    /*
     * tmp/temp is a workaround against a gcc compiler bug
     * http://veillard.com/gcc.bug
     */
    ret = 0;
    while ((CUR >= '0') && (CUR <= '9')) {
	ret = ret * 10;
	tmp = (CUR - '0');
        ok = 1;
        NEXT;
	temp = (double) tmp;
	ret = ret + temp;
    }
#else
    ret = 0;
    while ((CUR >= '0') && (CUR <= '9')) {
	ret = ret * 10 + (CUR - '0');
	ok = 1;
	NEXT;
    }
#endif
    if (CUR == '.') {
	int v, frac = 0, max;
	double fraction = 0;

        NEXT;
        if (((CUR < '0') || (CUR > '9')) && (!ok)) {
            xmlXPathCErr(ctxt, XPATH_NUMBER_ERROR);
            return(-1);
        }
        while (CUR == '0') {
            frac = frac + 1;
            NEXT;
        }
        max = frac + MAX_FRAC;
        while ((CUR >= '0') && (CUR <= '9') && (frac < max)) {
	    v = (CUR - '0');
	    fraction = fraction * 10 + v;
	    frac = frac + 1;
            NEXT;
        }
        fraction /= pow(10.0, frac);
        ret = ret + fraction;
        while ((CUR >= '0') && (CUR <= '9'))
            NEXT;
    }
    if ((CUR == 'e') || (CUR == 'E')) {
        NEXT;
        if (CUR == '-') {
            is_exponent_negative = 1;
            NEXT;
        } else if (CUR == '+') {
	    NEXT;
	}
        while ((CUR >= '0') && (CUR <= '9')) {
            if (exponent < 1000000)
                exponent = exponent * 10 + (CUR - '0');
            NEXT;
        }
        if (is_exponent_negative)
            exponent = -exponent;
        ret *= pow(10.0, (double) exponent);
    }

    opIndex = xmlXPathCompAdd(ctxt, &op, XPATH_OP_VALUE_NUMBER, XPATH_NUMBER);
    if (opIndex < 0)
        return(opIndex);
    op->as.number = ret;

    return(opIndex);
}

/**
 * xmlXPathParseLiteral:
 * @ctxt:  the XPath Parser context
 *
 * Parse a Literal
 *
 *  [29]   Literal ::=   '"' [^"]* '"'
 *                    | "'" [^']* "'"
 *
 * Returns the value found or NULL in case of error
 */
static xmlChar *
xmlXPathParseLiteral(xmlXPathContextPtr ctxt) {
    const xmlChar *q;
    xmlChar *ret = NULL;
    int quote;

    if (CUR == '"') {
        quote = '"';
    } else if (CUR == '\'') {
        quote = '\'';
    } else {
	xmlXPathCErr(ctxt, XPATH_START_LITERAL_ERROR);
        return(NULL);
    }

    NEXT;
    q = CUR_PTR;
    while (CUR != quote) {
        int ch;
        int len = 4;

        if (CUR == 0) {
            xmlXPathCErr(ctxt, XPATH_UNFINISHED_LITERAL_ERROR);
            return(NULL);
        }
        ch = xmlGetUTF8Char(CUR_PTR, &len);
        if ((ch < 0) || (IS_CHAR(ch) == 0)) {
            xmlXPathCErr(ctxt, XPATH_INVALID_CHAR_ERROR);
            return(NULL);
        }
        CUR_PTR += len;
    }
    ret = xmlStrndup(q, CUR_PTR - q);
    if (ret == NULL)
        xmlXPathErrMemory(ctxt);
    NEXT;
    return(ret);
}

/**
 * xmlXPathCompLiteral:
 * @ctxt:  the XPath Parser context
 *
 * Parse a Literal and push it on the stack.
 *
 *  [29]   Literal ::=   '"' [^"]* '"'
 *                    | "'" [^']* "'"
 *
 * TODO: xmlXPathCompLiteral memory allocation could be improved.
 */
static int
xmlXPathCompLiteral(xmlXPathContextPtr ctxt) {
    xmlXPathOpPtr op;
    xmlChar *string = NULL;
    int opIndex;

    string = xmlXPathParseLiteral(ctxt);
    if (string == NULL)
        return(-1);
    opIndex = xmlXPathCompAdd(ctxt, &op, XPATH_OP_VALUE_STRING, XPATH_STRING);
    if (opIndex < 0) {
        xmlFree(string);
        return(opIndex);
    }
    op->as.string = string;

    return(opIndex);
}

/**
 * xmlXPathCompVariableReference:
 * @ctxt:  the XPath Parser context
 *
 * Parse a VariableReference, evaluate it and push it on the stack.
 *
 * The variable bindings consist of a mapping from variable names
 * to variable values. The value of a variable is an object, which can be
 * of any of the types that are possible for the value of an expression,
 * and may also be of additional types not specified here.
 *
 * Early evaluation is possible since:
 * The variable bindings [...] used to evaluate a subexpression are
 * always the same as those used to evaluate the containing expression.
 *
 *  [36]   VariableReference ::=   '$' QName
 */
static int
xmlXPathCompVariableReference(xmlXPathContextPtr ctxt) {
    xmlXPathOpPtr op;
    xmlChar *name;
    xmlChar *prefix;
    const xmlChar *nsUri = NULL;
    int opIndex = -1;

    if (ctxt->pctxt.comp->flags & XML_XPATH_NOVAR) {
	xmlXPathCErr(ctxt, XPATH_FORBID_VARIABLE_ERROR);
        return(-1);
    }

    SKIP_BLANKS;
    if (CUR != '$') {
	xmlXPathCErr(ctxt, XPATH_VARIABLE_REF_ERROR);
        return(-1);
    }
    NEXT;
    name = xmlXPathParseQName(ctxt, &prefix);
    if (name == NULL) {
	xmlXPathCErr(ctxt, XPATH_VARIABLE_REF_ERROR);
        goto error;
    }

    if ((prefix != NULL) &&
        ((ctxt->pctxt.comp->flags & XML_XPATH_CHECKNS) ||
         (ctxt->pctxt.comp->flags & XML_XPATH_COMPILE_NS))) {
        nsUri = xmlXPathNsLookup(ctxt, prefix);
        if (nsUri == NULL) {
            xmlXPathCErr(ctxt, XPATH_UNDEF_PREFIX_ERROR);
        }

        if (ctxt->pctxt.comp->flags & XML_XPATH_CHECKNS)
            nsUri = NULL;
    }

    opIndex = xmlXPathCompAdd(ctxt, &op, XPATH_OP_VARIABLE, XPATH_UNDEFINED);
    if (opIndex < 0)
        goto error;

    if (xmlXPathCompOpSetQName(ctxt, &op->qname, name, prefix, nsUri) == 0) {
        name = NULL;
        prefix = NULL;
    } else {
        opIndex = -1;
    }

    SKIP_BLANKS;

error:
    if (name != NULL)
        xmlFree(name);
    if (prefix != NULL)
        xmlFree(prefix);

    return(opIndex);
}

/**
 * xmlXPathIsNodeType:
 * @name:  a name string
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Is the name given a NodeType one.
 *
 *  [38]   NodeType ::=   'comment'
 *                    | 'text'
 *                    | 'processing-instruction'
 *                    | 'node'
 *
 * Returns 1 if true 0 otherwise
 */
int
xmlXPathIsNodeType(const xmlChar *name) {
    if (name == NULL)
	return(0);

    if (xmlStrEqual(name, BAD_CAST "node"))
	return(1);
    if (xmlStrEqual(name, BAD_CAST "text"))
	return(1);
    if (xmlStrEqual(name, BAD_CAST "comment"))
	return(1);
    if (xmlStrEqual(name, BAD_CAST "processing-instruction"))
	return(1);
    return(0);
}

static int
xmlXPathTrueCompiler(xmlXPathContextPtr ctxt,
                     const xmlXPathStandardFunction *sfunc,
                     int nargs ATTRIBUTE_UNUSED,
                     int argIndex ATTRIBUTE_UNUSED) {
    xmlXPathOpPtr op;
    int opIndex;

    opIndex = xmlXPathCompAdd(ctxt, &op, XPATH_OP_VALUE_BOOL, XPATH_BOOLEAN);
    if (opIndex < 0)
        return(opIndex);

    op->as.boolean = (sfunc->op == XPATH_OP_TRUE);

    return(opIndex);
}

static int
xmlXPathNotCompiler(xmlXPathContextPtr ctxt,
                    const xmlXPathStandardFunction *sfunc ATTRIBUTE_UNUSED,
                    int nargs ATTRIBUTE_UNUSED, int argIndex) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathOpPtr argOp;

    argOp = &comp->steps[argIndex];

    if (argOp->op == XPATH_OP_BOOL) {
        argOp->op = XPATH_OP_NOT;
        return(argIndex);
    }

    return(xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_NOT, XPATH_BOOLEAN,
                                argIndex));
}

static int
xmlXPathNameCompiler(xmlXPathContextPtr ctxt,
                     const xmlXPathStandardFunction *sfunc, int nargs,
                     int argIndex) {
    int opIndex;

    if (nargs == 0) {
        opIndex = xmlXPathCompAdd(ctxt, NULL, sfunc->op + 1, XPATH_STRING);
    } else {
        xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
        xmlXPathOpPtr argOp;

        argOp = &comp->steps[argIndex];

        if (argOp->op == XPATH_OP_NODE) {
            opIndex = xmlXPathCompAdd(ctxt, NULL, sfunc->op + 1, XPATH_STRING);
        } else {
            /*
             * We only need the first node for name()
             */
            xmlXPathCompOpSetEvalMode(ctxt, argIndex, XPATH_EVAL_FIRST, 0);

            if (argOp->type != XPATH_NODESET) {
                if (argOp->type != XPATH_UNDEFINED) {
                    xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
                    return(-1);
                }
                argIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_NODESET,
                                                XPATH_NODESET, argIndex);
                if (argIndex < 0)
                    return(argIndex);
            }

            opIndex = xmlXPathCompAddUnary(ctxt, NULL, sfunc->op,
                                           XPATH_STRING, argIndex);
        }
    }

    return(opIndex);
}

/**
 * xmlXPathCompFunctionCall:
 * @ctxt:  the XPath Parser context
 *
 *  [16]   FunctionCall ::=   FunctionName '(' ( Argument ( ',' Argument)*)? ')'
 *  [17]   Argument ::=   Expr
 *
 * Compile a function call, the evaluation of all arguments are
 * pushed on the stack
 */
static int
xmlXPathCompFunctionCall(xmlXPathContextPtr ctxt) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathOpPtr op;
    xmlChar *name;
    xmlChar *prefix;
    const xmlChar *nsUri = NULL;
    const xmlXPathStandardFunction *sfunc = NULL;
    xmlXPathFunction func = NULL;
    int nbargs = 0;
    int flags;
    int sortArgs = 1;
    int opIndex = -1;
    int ch1, ch2;

    name = xmlXPathParseQName(ctxt, &prefix);
    if (name == NULL) {
	xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
        goto error;
    }
    SKIP_BLANKS;

    if (CUR != '(') {
	xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
        goto error;
    }
    NEXT;
    SKIP_BLANKS;

    flags = comp->flags;

    if (prefix != NULL) {
        if ((flags & XML_XPATH_CHECKNS) ||
            (flags & XML_XPATH_COMPILE_NS) ||
            (flags & XML_XPATH_COMPILE_FUNC)) {
            nsUri = xmlXPathNsLookup(ctxt, prefix);
            if (nsUri == NULL) {
                xmlXPathCErr(ctxt, XPATH_UNDEF_PREFIX_ERROR);
            }

            if (flags & XML_XPATH_COMPILE_FUNC) {
                func = xmlXPathFunctionLookupNS(ctxt, name, nsUri);
                if (func == NULL)
                    xmlXPathCErr(ctxt, XPATH_UNKNOWN_FUNC_ERROR);
            }

            if (flags & XML_XPATH_CHECKNS)
                nsUri = NULL;
        }
    } else {
        sfunc = xmlXPathLookupStandardFunction(name);

        if (sfunc != NULL) {
            func = sfunc->func;

            if ((sfunc->op == XPATH_OP_BOOL) ||
                (sfunc->op == XPATH_OP_NOT))
                sortArgs = 0;
        } else if (flags & XML_XPATH_COMPILE_FUNC) {
            func = xmlXPathFunctionLookupNS(ctxt, name, NULL);
            if (func == NULL)
                xmlXPathCErr(ctxt, XPATH_UNKNOWN_FUNC_ERROR);
        }
    }

    ch1 = -1;
    ch2 = -1;

    if (CUR != ')') {
	while (1) {
            xmlXPathObjectType type;

            if (ch1 != -1) {
                ch2 = xmlXPathCompAddBinary(ctxt, NULL, XPATH_OP_ARG, type, 
                                            ch1, ch2);
                if (ch2 < 0)
                    goto error;
            }

	    ch1 = xmlXPathCompileExpr(ctxt);
            if (ch1 < 0)
                goto error;

            if (sortArgs) {
                ch1 = xmlXPathCompAddSort(ctxt, ch1);
                if (ch1 < 0)
                    goto error;
            }

            if (sfunc != NULL) {
                if (nbargs == 0)
                    type = sfunc->arg1Type;
                else
                    type = sfunc->arg2Type;
            } else {
                type = XPATH_UNDEFINED;
            }

            ch1 = xmlXPathCompGetArg(ctxt, ch1, type);
            if (ch1 < 0)
                goto error;

	    nbargs++;
	    if (CUR == ')') break;
	    if (CUR != ',') {
                xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
                goto error;
	    }
	    NEXT;
	    SKIP_BLANKS;
	}
    }

    if ((sfunc != NULL) &&
        ((nbargs < sfunc->minArgs) || (nbargs > sfunc->maxArgs))) {
        xmlXPathCErr(ctxt, XPATH_INVALID_ARITY);
        goto error;
    }

    if ((sfunc != NULL) && (sfunc->compiler != NULL)) {
        opIndex = sfunc->compiler(ctxt, sfunc, nbargs, ch1);
    } else if ((sfunc != NULL) &&
               ((sfunc->op == XPATH_OP_BOOL) ||
                (sfunc->op == XPATH_OP_NUMBER) ||
                (sfunc->op == XPATH_OP_STRING)) &&
               (nbargs > 0)) {
        /* Already converted */
        opIndex = ch1;
    } else {
        int opcode;
        int type;

        if (sfunc != NULL) {
            opcode = sfunc->op;
            type = sfunc->retType;

            /*
             * TODO: Create extra ops for number() and string()
             * without args.
             */
            if ((opcode == XPATH_OP_NUMBER) || (opcode == XPATH_OP_STRING))
                opcode = XPATH_OP_SFUNC;
        } else {
            opcode = XPATH_OP_FUNCTION;
            type = XPATH_UNDEFINED;
        }

        opIndex = xmlXPathCompAddBinary(ctxt, &op, opcode, type, ch1, ch2);
        if (opIndex < 0)
            goto error;

        op->nbArgs = nbargs;
        op->as.func = func;

        /*
         * Standard functions only need the name for debug output.
         */
        if ((opcode == XPATH_OP_FUNCTION) ||
            (opcode == XPATH_OP_SFUNC) ||
            (opcode == XPATH_OP_SFUNC)) {
            if (xmlXPathCompOpSetQName(ctxt, &op->qname,
                                       name, prefix, nsUri) == 0) {
                name = NULL;
                prefix = NULL;
            } else {
                opIndex = -1;
            }
        }
    }

    NEXT;
    SKIP_BLANKS;

error:
    if (name != NULL)
        xmlFree(name);
    if (prefix != NULL)
        xmlFree(prefix);
    return(opIndex);
}

/**
 * xmlXPathCompPrimaryExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [15]   PrimaryExpr ::=   VariableReference
 *                | '(' Expr ')'
 *                | Literal
 *                | Number
 *                | FunctionCall
 *
 * Compile a primary expression.
 */
static int
xmlXPathCompPrimaryExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    SKIP_BLANKS;
    if (CUR == '$') {
        opIndex = xmlXPathCompVariableReference(ctxt);
    } else if (CUR == '(') {
	NEXT;
	SKIP_BLANKS;
	opIndex = xmlXPathCompileExpr(ctxt);
	if (opIndex < 0)
            return(-1);
	if (CUR != ')') {
	    xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
            return(-1);
	}
	NEXT;
	SKIP_BLANKS;
    } else if (IS_ASCII_DIGIT(CUR) || (CUR == '.' && IS_ASCII_DIGIT(NXT(1)))) {
	opIndex = xmlXPathCompNumber(ctxt);
    } else if ((CUR == '\'') || (CUR == '"')) {
	opIndex = xmlXPathCompLiteral(ctxt);
    } else {
	opIndex = xmlXPathCompFunctionCall(ctxt);
    }
    SKIP_BLANKS;

    return(opIndex);
}

/**
 * xmlXPathCompFilterExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [20]   FilterExpr ::=   PrimaryExpr
 *               | FilterExpr Predicate
 *
 * Compile a filter expression.
 * Square brackets are used to filter expressions in the same way that
 * they are used in location paths. It is an error if the expression to
 * be filtered does not evaluate to a node-set. The context node list
 * used for evaluating the expression in square brackets is the node-set
 * to be filtered listed in document order.
 */

static int
xmlXPathCompFilterExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompPrimaryExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);
    SKIP_BLANKS;

    if (CUR == '[') {
        /*
         * Filter input must be type-checked and sorted.
         */

        opIndex = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NODESET);
        if (opIndex < 0)
            return(opIndex);

        opIndex = xmlXPathCompAddSort(ctxt, opIndex);
        if (opIndex < 0)
            return(opIndex);

        while (CUR == '[') {
            opIndex = xmlXPathCompPredicate(ctxt, opIndex, 1);
            if (opIndex < 0)
                break;
            SKIP_BLANKS;
        }
    }

    return(opIndex);
}

/**
 * xmlXPathScanName:
 * @ctxt:  the XPath Parser context
 *
 * Trickery: parse an XML name but without consuming the input flow
 * Needed to avoid insanity in the parser state.
 *
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' |
 *                  CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * [6] Names ::= Name (S Name)*
 *
 * Returns the Name parsed or NULL
 */

static xmlChar *
xmlXPathScanName(xmlXPathContextPtr ctxt) {
    int l;
    int c;
    const xmlChar *cur;
    xmlChar *ret;

    cur = ctxt->pctxt.cur;

    c = CUR_CHAR(l);
    if ((c == ' ') || (c == '>') || (c == '/') || /* accelerators */
	(!IS_LETTER(c) && (c != '_') &&
         (c != ':'))) {
	return(NULL);
    }

    while ((c != ' ') && (c != '>') && (c != '/') && /* test bigname.xml */
	   ((IS_LETTER(c)) || (IS_DIGIT(c)) ||
            (c == '.') || (c == '-') ||
	    (c == '_') || (c == ':') ||
	    (IS_COMBINING(c)) ||
	    (IS_EXTENDER(c)))) {
	NEXTL(l);
	c = CUR_CHAR(l);
    }
    ret = xmlStrndup(cur, ctxt->pctxt.cur - cur);
    if (ret == NULL)
        xmlXPathErrMemory(ctxt);
    ctxt->pctxt.cur = cur;
    return(ret);
}

/**
 * xmlXPathCompPathExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [19]   PathExpr ::=   LocationPath
 *               | FilterExpr
 *               | FilterExpr '/' RelativeLocationPath
 *               | FilterExpr '//' RelativeLocationPath
 *
 * Compile a path expression.
 * The / operator and // operators combine an arbitrary expression
 * and a relative location path. It is an error if the expression
 * does not evaluate to a node-set.
 * The / operator does composition in the same way as when / is
 * used in a location path. As in location paths, // is short for
 * /descendant-or-self::node()/.
 */

static int
xmlXPathCompPathExpr(xmlXPathContextPtr ctxt) {
    int lc = 0;           /* Should we branch to LocationPath ?         */
    xmlChar *name = NULL; /* we may have to preparse a name to find out */
    int opIndex;

    SKIP_BLANKS;
    if ((CUR == '*') || (CUR == '/') || (CUR == '@') ||
	((CUR == '.') && (!IS_ASCII_DIGIT(NXT(1))))) {
	lc = 1;
    } else {
	/*
	 * Problem is finding if we have a name here whether it's:
	 *   - a nodetype
	 *   - a function call in which case it's followed by '('
	 *   - an axis in which case it's followed by ':'
	 *   - a element name
	 * We do an a priori analysis here rather than having to
	 * maintain parsed token content through the recursive function
	 * calls. This looks uglier but makes the code easier to
	 * read/write/debug.
	 */
	SKIP_BLANKS;
	name = xmlXPathScanName(ctxt);
        if (ctxt->pctxt.error)
            return(-1);

        if (name != NULL) {
            if (xmlStrstr(name, (xmlChar *) "::") != NULL) {
                lc = 1;
            } else {
                int len =xmlStrlen(name);

                while (IS_BLANK_CH(NXT(len)))
                    len++;

                if ((NXT(len) != '(')) {
                    lc = 1;
                } else if (xmlXPathIsNodeType(name)) {
                    lc = 1;
                }
            }

	    xmlFree(name);
	}
    }

    if (lc) {
	if (CUR == '/') {
	    opIndex = xmlXPathCompAdd(ctxt, NULL, XPATH_OP_ROOT,
                                      XPATH_NODESET);
	} else {
	    opIndex = xmlXPathCompAdd(ctxt, NULL, XPATH_OP_NODE,
                                      XPATH_NODESET);
	}
	opIndex = xmlXPathCompLocationPath(ctxt, opIndex);
    } else {
	opIndex = xmlXPathCompFilterExpr(ctxt);
	if (opIndex < 0)
            return(opIndex);
	if ((CUR == '/') && (NXT(1) == '/')) {
	    SKIP(2);
	    SKIP_BLANKS;

            opIndex = xmlXPathCompAddStep(ctxt, NULL, opIndex, -1,
                                          AXIS_DESCENDANT_OR_SELF,
                                          TYPE_MASK_NODE);
            if (opIndex < 0)
                return(opIndex);

	    opIndex = xmlXPathCompRelativeLocationPath(ctxt, opIndex);
	} else if (CUR == '/') {
	    opIndex = xmlXPathCompRelativeLocationPath(ctxt, opIndex);
	}
    }
    SKIP_BLANKS;

    return(opIndex);
}

/**
 * xmlXPathCompUnionExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [18]   UnionExpr ::=   PathExpr
 *               | UnionExpr '|' PathExpr
 *
 * Compile an union expression.
 */

static int
xmlXPathCompUnionExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompPathExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while (CUR == '|') {
	int ch1, ch2;

        ch1 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NODESET);
        if (ch1 < 0)
            return(opIndex);

	NEXT;
	SKIP_BLANKS;
	opIndex = xmlXPathCompPathExpr(ctxt);
        if (opIndex < 0)
            return(opIndex);

        ch2 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NODESET);
        if (ch2 < 0)
            return(ch2);

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, XPATH_OP_UNION,
                                        XPATH_NODESET, ch1, ch2);
        if (opIndex < 0)
            break;

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompUnaryExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [27]   UnaryExpr ::=   UnionExpr
 *                   | '-' UnaryExpr
 *
 * Compile an unary expression.
 */

static int
xmlXPathCompUnaryExpr(xmlXPathContextPtr ctxt) {
    int opIndex;
    int minus = 0;

    SKIP_BLANKS;
    while (CUR == '-') {
        minus = 1 - minus;
	NEXT;
	SKIP_BLANKS;
    }

    opIndex = xmlXPathCompUnionExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    if (minus) {
        opIndex = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NUMBER);
        if (opIndex < 0)
            return(opIndex);

        opIndex = xmlXPathCompAddUnary(ctxt, NULL, XPATH_OP_NEG,
                                       XPATH_NUMBER, opIndex);
    }

    return(opIndex);
}

/**
 * xmlXPathCompMultiplicativeExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [26]   MultiplicativeExpr ::=   UnaryExpr
 *                   | MultiplicativeExpr MultiplyOperator UnaryExpr
 *                   | MultiplicativeExpr 'div' UnaryExpr
 *                   | MultiplicativeExpr 'mod' UnaryExpr
 *  [34]   MultiplyOperator ::=   '*'
 *
 * Compile an Additive expression.
 */

static int
xmlXPathCompMultiplicativeExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompUnaryExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while ((CUR == '*') ||
           ((CUR == 'd') && (NXT(1) == 'i') && (NXT(2) == 'v')) ||
           ((CUR == 'm') && (NXT(1) == 'o') && (NXT(2) == 'd'))) {
        xmlXPathOpcode opcode;
	int ch1, ch2;

        ch1 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NUMBER);
        if (ch1 < 0)
            return(ch1);

        if (CUR == '*') {
	    opcode = XPATH_OP_MULT;
	    NEXT;
	} else if (CUR == 'd') {
	    opcode = XPATH_OP_DIV;
	    SKIP(3);
	} else {
	    opcode = XPATH_OP_MOD;
	    SKIP(3);
	}
	SKIP_BLANKS;

        opIndex = xmlXPathCompUnaryExpr(ctxt);
	if (opIndex < 0)
	    return(opIndex);

        ch2 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NUMBER);
        if (ch2 < 0)
            return(ch2);

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, opcode, XPATH_NUMBER,
                                        ch1, ch2);
        if (opIndex < 0)
            break;

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompAdditiveExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [25]   AdditiveExpr ::=   MultiplicativeExpr
 *                   | AdditiveExpr '+' MultiplicativeExpr
 *                   | AdditiveExpr '-' MultiplicativeExpr
 *
 * Compile an Additive expression.
 */

static int
xmlXPathCompAdditiveExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompMultiplicativeExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while ((CUR == '+') || (CUR == '-')) {
        xmlXPathOpcode opcode;
	int ch1, ch2;

        ch1 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NUMBER);
        if (ch1 < 0)
            return(ch1);

        if (CUR == '+') opcode = XPATH_OP_ADD;
	else opcode = XPATH_OP_SUB;
	NEXT;
	SKIP_BLANKS;

        opIndex = xmlXPathCompMultiplicativeExpr(ctxt);
	if (opIndex < 0)
	    return(opIndex);

        ch2 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_NUMBER);
        if (ch2 < 0)
            return(ch2);

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, opcode, XPATH_NUMBER,
                                        ch1, ch2);
        if (opIndex < 0)
            break;

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompRelationalExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [24]   RelationalExpr ::=   AdditiveExpr
 *                 | RelationalExpr '<' AdditiveExpr
 *                 | RelationalExpr '>' AdditiveExpr
 *                 | RelationalExpr '<=' AdditiveExpr
 *                 | RelationalExpr '>=' AdditiveExpr
 *
 *  A <= B > C is allowed ? Answer from James, yes with
 *  (AdditiveExpr <= AdditiveExpr) > AdditiveExpr
 *  which is basically what got implemented.
 *
 * Compile a Relational expression, then push the result
 * on the stack
 */

static int
xmlXPathCompRelationalExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompAdditiveExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while ((CUR == '<') || (CUR == '>')) {
        xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
        xmlXPathOpPtr op1, op2;
        xmlXPathOpcode opcode;
        int less, strict = 1;
	int ch1 = opIndex;
        int ch2;

        less = (CUR == '<');
	NEXT;
        if (CUR == '=') {
            strict = 0;
            NEXT;
        }
	SKIP_BLANKS;

        ch2 = xmlXPathCompAdditiveExpr(ctxt);
	if (ch2 < 0)
	    return(ch2);

        op1 = &comp->steps[ch1];
        op2 = &comp->steps[ch2];

        if (((op1->type == XPATH_BOOLEAN) ||
             (op1->type == XPATH_NUMBER) ||
             (op1->type == XPATH_STRING)) &&
            ((op2->type == XPATH_BOOLEAN) ||
             (op2->type == XPATH_NUMBER) ||
             (op2->type == XPATH_STRING))) {
            ch1 = xmlXPathCompGetArg(ctxt, ch1, XPATH_NUMBER);
            ch2 = xmlXPathCompGetArg(ctxt, ch2, XPATH_NUMBER);

            opcode = strict ? XPATH_OP_LT_NUM : XPATH_OP_LE_NUM;
        } else {
            opcode = strict ? XPATH_OP_LT : XPATH_OP_LE;
        }

        if (!less) {
            int tmp = ch1;
            ch1 = ch2;
            ch2 = tmp;
        }

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, opcode, XPATH_BOOLEAN,
                                        ch1, ch2);
        if (opIndex < 0)
            break;

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompEqualityExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [23]   EqualityExpr ::=   RelationalExpr
 *                 | EqualityExpr '=' RelationalExpr
 *                 | EqualityExpr '!=' RelationalExpr
 *
 *  A != B != C is allowed ? Answer from James, yes with
 *  (RelationalExpr = RelationalExpr) = RelationalExpr
 *  (RelationalExpr != RelationalExpr) != RelationalExpr
 *  which is basically what got implemented.
 *
 * Compile an Equality expression.
 *
 */
static int
xmlXPathCompEqualityExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompRelationalExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while ((CUR == '=') || ((CUR == '!') && (NXT(1) == '='))) {
        xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
        xmlXPathOpPtr op1, op2;
	xmlXPathOpcode opcode;
        int neq;
	int ch1 = opIndex;
        int ch2;

        if (CUR == '=') {
            neq = 0;
        } else {
            neq = 1;
            NEXT;
        }
	NEXT;
	SKIP_BLANKS;
        ch2 = xmlXPathCompRelationalExpr(ctxt);
	if (ch2 < 0)
	    return(ch2);

        op1 = &comp->steps[ch1];
        op2 = &comp->steps[ch2];

        if ((op1->type == XPATH_STRING) && (op2->type == XPATH_STRING)) {
            ch1 = xmlXPathCompGetArg(ctxt, ch1, XPATH_STRING);
            if (ch1 < 0)
                return(ch1);
            ch2 = xmlXPathCompGetArg(ctxt, ch2, XPATH_STRING);
            if (ch2 < 0)
                return(ch2);

            opcode = neq ? XPATH_OP_NE_STR : XPATH_OP_EQ_STR;
        } else if (((op1->type == XPATH_NUMBER) ||
                    (op1->type == XPATH_STRING)) &&
                   ((op2->type == XPATH_NUMBER) ||
                    (op2->type == XPATH_STRING))) {
            ch1 = xmlXPathCompGetArg(ctxt, ch1, XPATH_NUMBER);
            if (ch1 < 0)
                return(ch1);
            ch2 = xmlXPathCompGetArg(ctxt, ch2, XPATH_NUMBER);
            if (ch2 < 0)
                return(ch2);

            opcode = neq ? XPATH_OP_NE_NUM : XPATH_OP_EQ_NUM;
        } else {
            opcode = neq ? XPATH_OP_NE : XPATH_OP_EQ;
        }

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, opcode, XPATH_BOOLEAN,
                                        ch1, ch2);
        if (opIndex < 0)
            break;

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompAndExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [22]   AndExpr ::=   EqualityExpr
 *                 | AndExpr 'and' EqualityExpr
 *
 * Compile an AND expression.
 *
 */
static int
xmlXPathCompAndExpr(xmlXPathContextPtr ctxt) {
    int opIndex;

    opIndex = xmlXPathCompEqualityExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while ((CUR == 'a') && (NXT(1) == 'n') && (NXT(2) == 'd')) {
	int ch1, ch2;

        ch1 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_BOOLEAN);
        if (ch1 < 0)
            return(ch1);

        SKIP(3);
	SKIP_BLANKS;

        opIndex = xmlXPathCompEqualityExpr(ctxt);
        if (opIndex < 0)
            return(opIndex);

        ch2 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_BOOLEAN);
        if (ch2 < 0)
            return(ch2);

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, XPATH_OP_AND,
                                        XPATH_BOOLEAN, ch1, ch2);
        if (opIndex < 0)
            break;

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompileExpr:
 * @ctxt:  the XPath Parser context
 *
 *  [14]   Expr ::=   OrExpr
 *  [21]   OrExpr ::=   AndExpr
 *                 | OrExpr 'or' AndExpr
 *
 * Parse and compile an expression
 */
static int
xmlXPathCompileExpr(xmlXPathContextPtr ctxt) {
    int opIndex;
    int ret = -1;

    if (ctxt->depth >= XPATH_MAX_RECURSION_DEPTH) {
        xmlXPathCErr(ctxt, XPATH_RECURSION_LIMIT_EXCEEDED);
        return(-1);
    }

    /*
     * Parsing subexpressions can result in more than 10 function calls
     * before recursing!
     *
     * Expr (48) ->
     * AndExp (48) ->
     * EqualityExpr (64) ->
     * RelationalExpr (80) ->
     * AdditiveExpr (64) ->
     * MultiplicativeExpr (64) ->
     * UnaryExpr (64) ->
     * UnionExpr (0 if inlined) ->
     * PathExpr (128) ->
     * LocationPath (0) ->
     * RelactiveLocationPath (64) ->
     * Step (96) ->
     * Predicate (64) ->
     * Expr
     */
    ctxt->depth += 8;

    opIndex = xmlXPathCompAndExpr(ctxt);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while ((CUR == 'o') && (NXT(1) == 'r')) {
	int ch1, ch2;

        ch1 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_BOOLEAN);
        if (ch1 < 0)
            goto error;

        SKIP(2);
	SKIP_BLANKS;

        opIndex = xmlXPathCompAndExpr(ctxt);
	if (opIndex < 0)
	    goto error;

        ch2 = xmlXPathCompGetArg(ctxt, opIndex, XPATH_BOOLEAN);
        if (ch2 < 0)
            goto error;

        opIndex = xmlXPathCompAddBinary(ctxt, NULL, XPATH_OP_OR,
                                        XPATH_BOOLEAN, ch1, ch2);
        if (opIndex < 0)
            goto error;

	SKIP_BLANKS;
    }

    ret = opIndex;

error:
    ctxt->depth -= 10;

    return(ret);
}

/**
 * xmlXPathPredicateEvalMode:
 * @ctxt:  parser context
 * @opIndex:  index of predicate op
 *
 * Optimize evaluation of predicates [1], [n] or [last()].
 *
 * Returns the evaluation mode for a predicate.
 */
static xmlXPathEvalMode
xmlXPathPredicateEvalMode(xmlXPathContextPtr ctxt, int opIndex) {
    xmlXPathOpPtr steps = ctxt->pctxt.comp->steps;
    xmlXPathOpPtr pred;
    xmlXPathEvalMode mode = XPATH_EVAL_ALL;

    pred = &steps[opIndex];

    /*
     * Also check for [position()=x]
     */
    if (pred->op == XPATH_OP_EQ_NUM) {
        if (steps[pred->ch1].op == XPATH_OP_POSITION)
            pred = &steps[pred->ch2];
        else if (steps[pred->ch2].op == XPATH_OP_POSITION)
            pred = &steps[pred->ch1];
    }

    if (pred->op == XPATH_OP_VALUE_NUMBER) {
        double floatval = pred->as.number;

        if ((floatval > 0.0) && (floatval < XPATH_EVAL_LAST)) {
            int index = floatval;

            /* Check whether floatval is an integer */
            if (index == floatval)
                mode = index;
        }
    } else if (pred->op == XPATH_OP_LAST) {
        mode = XPATH_EVAL_LAST;
    }

    return(mode);
}

/**
 * xmlXPathCompPredicate:
 * @ctxt:  the XPath Parser context
 * @filter:  act as a filter
 *
 *  [8]   Predicate ::=   '[' PredicateExpr ']'
 *  [9]   PredicateExpr ::=   Expr
 *
 * Compile a predicate expression
 *
 * Returns the index of the resulting operation.
 */
static int
xmlXPathCompPredicate(xmlXPathContextPtr ctxt, int argIndex, int filter) {
    xmlXPathEvalMode mode;
    xmlXPathOpcode opcode;
    int predIndex;

    argIndex = xmlXPathCompGetArg(ctxt, argIndex, XPATH_NODESET);
    if (argIndex < 0)
        return(-1);

    SKIP_BLANKS;
    if (CUR != '[') {
	xmlXPathCErr(ctxt, XPATH_INVALID_PREDICATE_ERROR);
        return(-1);
    }
    NEXT;
    SKIP_BLANKS;

    predIndex = xmlXPathCompileExpr(ctxt);
    if (predIndex < 0)
        return(predIndex);

    if (CUR != ']') {
	xmlXPathCErr(ctxt, XPATH_INVALID_PREDICATE_ERROR);
        return(-1);
    }

    NEXT;
    SKIP_BLANKS;

    xmlXPathCompOpSetEvalMode(ctxt, predIndex, XPATH_EVAL_ANY, 0);

    mode = xmlXPathPredicateEvalMode(ctxt, predIndex);

    if (mode != XPATH_EVAL_ALL) {
        xmlXPathCompOpSetEvalMode(ctxt, argIndex, mode, 1);

        /* Ignore filter op */
        return(argIndex);
    } else {
        if (filter)
            opcode = XPATH_OP_FILTER;
        else
            opcode = XPATH_OP_PREDICATE;

        return(xmlXPathCompAddBinary(ctxt, NULL, opcode, XPATH_NODESET,
                                     argIndex, predIndex));
    }
}

/**
 * xmlXPathCompNodeTest:
 * @ctxt:  the XPath Parser context
 * @type:  pointer to int
 * @prefix:  placeholder for a possible name prefix
 * @namePtr:  pointer to pre-parsed name (in/out)
 *
 * [7] NodeTest ::=   NameTest
 *		    | NodeType '(' ')'
 *		    | 'processing-instruction' '(' Literal ')'
 *
 * [37] NameTest ::=  '*'
 *		    | NCName ':' '*'
 *		    | QName
 * [38] NodeType ::= 'comment'
 *		   | 'text'
 *		   | 'processing-instruction'
 *		   | 'node'
 *
 * Updates @type, @prefix and @namePtr appropriately.
 */
static int
xmlXPathCompNodeTest(xmlXPathContextPtr ctxt, int *type,
                     xmlChar **prefix, xmlChar **namePtr) {
    xmlChar *name = *namePtr;
    int blanks;

    *type = 0;
    *prefix = NULL;
    *namePtr = NULL;
    SKIP_BLANKS;

    if ((name == NULL) && (CUR == '*')) {
	/*
	 * All elements
	 */
	NEXT;
	return(0);
    }

    if (name == NULL) {
	name = xmlXPathParseNameInternal(ctxt, ':');
        if (name == NULL) {
            xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
            return(-1);
        }
    }

    blanks = IS_BLANK_CH(CUR);
    SKIP_BLANKS;
    if (CUR == '(') {
	NEXT;
	/*
	 * NodeType or PI search
	 */
	if (xmlStrEqual(name, BAD_CAST "comment"))
	    *type = TYPE_MASK_COMMENT;
	else if (xmlStrEqual(name, BAD_CAST "node"))
	    *type = TYPE_MASK_NODE;
	else if (xmlStrEqual(name, BAD_CAST "processing-instruction"))
	    *type = TYPE_MASK_PI;
	else if (xmlStrEqual(name, BAD_CAST "text"))
	    *type = TYPE_MASK_TEXT;
	else {
	    xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
	    xmlFree(name);
            return(-1);
	}

	xmlFree(name);

	SKIP_BLANKS;
	if (*type == TYPE_MASK_PI) {
	    /*
	     * Specific case: search a PI by name.
	     */
	    if (CUR != ')') {
		*namePtr = xmlXPathParseLiteral(ctxt);
		SKIP_BLANKS;
	    }
	}
	if (CUR != ')') {
	    xmlXPathCErr(ctxt, XPATH_UNCLOSED_ERROR);
            return(-1);
	}
	NEXT;
	return(0);
    }

    if ((!blanks) && (CUR == ':')) {
	NEXT;

	/*
	 * Since currently the parser context don't have a
	 * namespace list associated:
	 * The namespace name for this prefix can be computed
	 * only at evaluation time. The compilation is done
	 * outside of any context.
	 */
	*prefix = name;

	if (CUR == '*') {
	    /*
	     * All elements
	     */
            name = xmlStrdup(BAD_CAST "*");
            if (name == NULL) {
                xmlXPathErrMemory(ctxt);
                return(-1);
            }
	    NEXT;
	} else {
	    name = xmlXPathParseNameInternal(ctxt, ':');
            if (name == NULL) {
                xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
                return(-1);
            }
        }
    }

    *namePtr = name;

    return(0);
}

/**
 * xmlXPathIsAxisName:
 * @name:  a preparsed name token
 *
 * [6] AxisName ::=   'ancestor'
 *                  | 'ancestor-or-self'
 *                  | 'attribute'
 *                  | 'child'
 *                  | 'descendant'
 *                  | 'descendant-or-self'
 *                  | 'following'
 *                  | 'following-sibling'
 *                  | 'namespace'
 *                  | 'parent'
 *                  | 'preceding'
 *                  | 'preceding-sibling'
 *                  | 'self'
 *
 * Returns the axis or 0
 */
static xmlXPathAxisVal
xmlXPathIsAxisName(const xmlChar *name) {
    xmlXPathAxisVal ret = (xmlXPathAxisVal) 0;
    switch (name[0]) {
	case 'a':
	    if (xmlStrEqual(name, BAD_CAST "ancestor"))
		ret = AXIS_ANCESTOR;
	    if (xmlStrEqual(name, BAD_CAST "ancestor-or-self"))
		ret = AXIS_ANCESTOR_OR_SELF;
	    if (xmlStrEqual(name, BAD_CAST "attribute"))
		ret = AXIS_ATTRIBUTE;
	    break;
	case 'c':
	    if (xmlStrEqual(name, BAD_CAST "child"))
		ret = AXIS_CHILD;
	    break;
	case 'd':
	    if (xmlStrEqual(name, BAD_CAST "descendant"))
		ret = AXIS_DESCENDANT;
	    if (xmlStrEqual(name, BAD_CAST "descendant-or-self"))
		ret = AXIS_DESCENDANT_OR_SELF;
	    break;
	case 'f':
	    if (xmlStrEqual(name, BAD_CAST "following"))
		ret = AXIS_FOLLOWING;
	    if (xmlStrEqual(name, BAD_CAST "following-sibling"))
		ret = AXIS_FOLLOWING_SIBLING;
	    break;
	case 'n':
	    if (xmlStrEqual(name, BAD_CAST "namespace"))
		ret = AXIS_NAMESPACE;
	    break;
	case 'p':
	    if (xmlStrEqual(name, BAD_CAST "parent"))
		ret = AXIS_PARENT;
	    if (xmlStrEqual(name, BAD_CAST "preceding"))
		ret = AXIS_PRECEDING;
	    if (xmlStrEqual(name, BAD_CAST "preceding-sibling"))
		ret = AXIS_PRECEDING_SIBLING;
	    break;
	case 's':
	    if (xmlStrEqual(name, BAD_CAST "self"))
		ret = AXIS_SELF;
	    break;
    }
    return(ret);
}

/**
 * xmlXPathCompStep:
 * @ctxt:  the XPath Parser context
 *
 * [4] Step ::=   AxisSpecifier NodeTest Predicate*
 *                  | AbbreviatedStep
 *
 * [12] AbbreviatedStep ::=   '.' | '..'
 *
 * [5] AxisSpecifier ::= AxisName '::'
 *                  | AbbreviatedAxisSpecifier
 *
 * [13] AbbreviatedAxisSpecifier ::= '@'?
 *
 * Modified for XPtr range support as:
 *
 *  [4xptr] Step ::= AxisSpecifier NodeTest Predicate*
 *                     | AbbreviatedStep
 *                     | 'range-to' '(' Expr ')' Predicate*
 *
 * Compile one step in a Location Path
 * A location step of . is short for self::node(). This is
 * particularly useful in conjunction with //. For example, the
 * location path .//para is short for
 * self::node()/descendant-or-self::node()/child::para
 * and so will select all para descendant elements of the context
 * node.
 * Similarly, a location step of .. is short for parent::node().
 * For example, ../title is short for parent::node()/child::title
 * and so will select the title children of the parent of the context
 * node.
 */
static int
xmlXPathCompStep(xmlXPathContextPtr ctxt, int argIndex) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    int ret = -1;

    SKIP_BLANKS;
    if ((CUR == '.') && (NXT(1) == '.')) {
	SKIP(2);
	SKIP_BLANKS;

	ret = xmlXPathCompAddStep(ctxt, NULL, argIndex, -1,
                                  AXIS_PARENT, TYPE_MASK_NODE);
    } else if (CUR == '.') {
	NEXT;
	SKIP_BLANKS;

        ret = argIndex;
    } else {
        xmlXPathOpPtr op;
	xmlChar *name = NULL;
	xmlChar *prefix = NULL;
        const xmlChar *nsUri = NULL;
	xmlXPathAxisVal axis = (xmlXPathAxisVal) 0;
	int type = 0;
        int opIndex, predIndex, lastIndex;

	if (CUR == '*') {
	    axis = AXIS_CHILD;
	} else {
	    if (name == NULL) {
		name = xmlXPathParseNameInternal(ctxt, ':');
                if (ctxt->pctxt.error != XPATH_EXPRESSION_OK)
                    goto error;
            }
	    if (name != NULL) {
		axis = xmlXPathIsAxisName(name);
		if (axis != 0) {
		    SKIP_BLANKS;
		    if ((CUR == ':') && (NXT(1) == ':')) {
			SKIP(2);
			xmlFree(name);
			name = NULL;
		    } else {
			/* an element name can conflict with an axis one :-\ */
			axis = AXIS_CHILD;
		    }
		} else {
		    axis = AXIS_CHILD;
		}
	    } else if (CUR == '@') {
		NEXT;
		axis = AXIS_ATTRIBUTE;
	    } else {
		axis = AXIS_CHILD;
	    }
	}

	if (xmlXPathCompNodeTest(ctxt, &type, &prefix, &name) < 0)
            goto error;

        if (type == 0) {
            /* principal node type */

            if (axis == AXIS_ATTRIBUTE)
                type = TYPE_MASK_ATTR;
            else if (axis == AXIS_NAMESPACE)
                type = TYPE_MASK_NS;
            else
                type = TYPE_MASK_ELEM;
        }

        if ((prefix != NULL) &&
	    ((comp->flags & XML_XPATH_CHECKNS) ||
             (comp->flags & XML_XPATH_COMPILE_NS))) {
	    nsUri = xmlXPathNsLookup(ctxt, prefix);
            if (nsUri == NULL) {
		xmlXPathCErr(ctxt, XPATH_UNDEF_PREFIX_ERROR);
	    }

            if (comp->flags & XML_XPATH_CHECKNS)
                nsUri = NULL;
	}

        opIndex = xmlXPathCompAddStep(ctxt, &op, argIndex, -1, axis, type);
        if (opIndex < 0)
            goto error;
        if ((name != NULL) &&
            (xmlXPathCompOpSetQName(ctxt, &op->qname,
                                   name, prefix, nsUri) == 0)) {
            prefix = NULL;
            name = NULL;
        }

        /*
         * We can't reuse op since the steps array might be reallocated.
         */
        op = NULL;

	SKIP_BLANKS;
        predIndex = -1;
        lastIndex = opIndex;
	while (CUR == '[') {
	    lastIndex = xmlXPathCompPredicate(ctxt, lastIndex, 0);
            if (lastIndex < 0)
                goto error;

            if (lastIndex != opIndex) {
                if (predIndex == -1) {
                    /* Unlink predicate chain */
                    comp->steps[lastIndex].ch1 = -1;
                }

                predIndex = lastIndex;
            }
	}

        /* Relink predicate chain */
        comp->steps[opIndex].ch2 = predIndex;

        ret = opIndex;

error:
        xmlFree(prefix);
        xmlFree(name);
    }

    return(ret);
}

/**
 * xmlXPathCompRelativeLocationPath:
 * @ctxt:  the XPath Parser context
 *
 *  [3]   RelativeLocationPath ::=   Step
 *                     | RelativeLocationPath '/' Step
 *                     | AbbreviatedRelativeLocationPath
 *  [11]  AbbreviatedRelativeLocationPath ::=   RelativeLocationPath '//' Step
 *
 * Compile a relative location path.
 */
static int
xmlXPathCompRelativeLocationPath(xmlXPathContextPtr ctxt, int opIndex) {
    SKIP_BLANKS;
    if ((CUR == '/') && (NXT(1) == '/')) {
	SKIP(2);
	SKIP_BLANKS;

	opIndex = xmlXPathCompAddStep(ctxt, NULL, opIndex, -1,
                                      AXIS_DESCENDANT_OR_SELF, TYPE_MASK_NODE);
        if (opIndex < 0)
            return(-1);
    } else if (CUR == '/') {
	NEXT;
	SKIP_BLANKS;
    }

    opIndex = xmlXPathCompStep(ctxt, opIndex);
    if (opIndex < 0)
        return(opIndex);

    SKIP_BLANKS;
    while (CUR == '/') {
	if ((CUR == '/') && (NXT(1) == '/')) {
	    SKIP(2);
	    SKIP_BLANKS;

            opIndex = xmlXPathCompAddStep(ctxt, NULL, opIndex, -1,
                                          AXIS_DESCENDANT_OR_SELF,
                                          TYPE_MASK_NODE);
            if (opIndex < 0)
                return(opIndex);

	    opIndex = xmlXPathCompStep(ctxt, opIndex);
	} else if (CUR == '/') {
	    NEXT;
	    SKIP_BLANKS;
	    opIndex = xmlXPathCompStep(ctxt, opIndex);
	}
        if (opIndex < 0)
            return(opIndex);

	SKIP_BLANKS;
    }

    return(opIndex);
}

/**
 * xmlXPathCompLocationPath:
 * @ctxt:  the XPath Parser context
 *
 *  [1]   LocationPath ::=   RelativeLocationPath
 *                     | AbsoluteLocationPath
 *  [2]   AbsoluteLocationPath ::=   '/' RelativeLocationPath?
 *                     | AbbreviatedAbsoluteLocationPath
 *  [10]   AbbreviatedAbsoluteLocationPath ::=
 *                           '//' RelativeLocationPath
 *
 * Compile a location path
 *
 * // is short for /descendant-or-self::node()/. For example,
 * //para is short for /descendant-or-self::node()/child::para and
 * so will select any para element in the document (even a para element
 * that is a document element will be selected by //para since the
 * document element node is a child of the root node); div//para is
 * short for div/descendant-or-self::node()/child::para and so will
 * select all para descendants of div children.
 */
static int
xmlXPathCompLocationPath(xmlXPathContextPtr ctxt, int opIndex) {
    SKIP_BLANKS;
    if (CUR != '/') {
        opIndex = xmlXPathCompRelativeLocationPath(ctxt, opIndex);
    } else {
	while (CUR == '/') {
	    if ((CUR == '/') && (NXT(1) == '/')) {
		SKIP(2);
		SKIP_BLANKS;

                opIndex = xmlXPathCompAddStep(ctxt, NULL, opIndex, -1,
                                              AXIS_DESCENDANT_OR_SELF,
                                              TYPE_MASK_NODE);
                if (opIndex < 0)
                    return(opIndex);

		opIndex = xmlXPathCompRelativeLocationPath(ctxt, opIndex);
	    } else if (CUR == '/') {
		NEXT;
		SKIP_BLANKS;
		if ((CUR != 0) &&
		    ((IS_ASCII_LETTER(CUR)) || (CUR >= 0x80) ||
                     (CUR == '_') || (CUR == '.') ||
		     (CUR == '@') || (CUR == '*')))
		    opIndex = xmlXPathCompRelativeLocationPath(ctxt, opIndex);
	    }
	    if (opIndex < 0)
	        break;
	}
    }

    return(opIndex);
}

/************************************************************************
 *									*
 *		XPath precompiled expression evaluation			*
 *									*
 ************************************************************************/

static int
xmlXPathCompOpEvalNodeset(xmlXPathContextPtr ctxt, xmlXPathItem *result,
                          const xmlXPathOp *op, xmlXPathEvalMode mode) {
    xmlXPathEvalMode opMode;

    if (mode == XPATH_EVAL_DEFAULT)
        opMode = op->mode;
    else
        opMode = mode;

    if ((opMode == XPATH_EVAL_ALL) || (opMode == XPATH_EVAL_ANY))
        return(0);

    if (result->isCopy) {
        if (xmlXPathItemCloneNodeSet(ctxt, result) < 0)
            return(-1);
    }

    switch (opMode) {
        case XPATH_EVAL_NONE:
            xmlXPathNodeSetClear(&result->as.nodeset, result->hasNsNodes);
            break;

        case XPATH_EVAL_FIRST:
            if (result->as.nodeset.nodeNr > 1)
                xmlXPathNodeSetKeep(result, 0);
            break;

        case XPATH_EVAL_LAST:
            if (result->as.nodeset.nodeNr > 1)
                xmlXPathNodeSetKeep(result, result->as.nodeset.nodeNr - 1);
            break;

        default:
            if (result->as.nodeset.nodeNr > 0)
                xmlXPathNodeSetKeep(result, opMode - 1);
            break;
    }

    return(0);
}

/**
 * xmlXPathNodeSetFilter:
 * @ctxt:  the XPath Parser context
 * @op:  the filter or predicate op
 * @set:  the node set to filter
 * @offset:  filter nodes starting at offset
 * @mode:  evaluation mode or XPATH_EVAL_DEFAULT
 * @hasNsNodes:  true if the node set may contain namespace nodes
 *
 * Filter a node set, keeping only nodes for which the predicate expression
 * matches. This takes the evaluation mode into account.
 */
ATTRIBUTE_NO_INLINE
static void
xmlXPathNodeSetFilter(xmlXPathContextPtr ctxt, const xmlXPathOp *op,
		      xmlNodeSetPtr set, int offset, xmlXPathEvalMode mode,
                      int hasNsNodes) {
    xmlNodePtr oldnode, *nodes, hit;
    int oldcs, oldpp;
    int numNodes;
    int i, j, pos, incr;
    int breakPos;

    if ((set == NULL) || (set->nodeNr <= offset))
        return;

    if (mode == XPATH_EVAL_DEFAULT)
        mode = op->mode;

    if (mode == XPATH_EVAL_NONE)
        return;

    oldnode = ctxt->node;
    oldcs = ctxt->contextSize;
    oldpp = ctxt->proximityPosition;

    nodes = set->nodeTab + offset;
    numNodes = set->nodeNr - offset;
    ctxt->contextSize = numNodes;

    hit = NULL;
    j = 0;

    if (mode == XPATH_EVAL_LAST) {
        i = numNodes - 1;
        incr = -1;
        breakPos = 1;
    } else {
        i = 0;
        incr = 1;

        if (mode == XPATH_EVAL_ALL) {
            breakPos = 0;
        } else {
            if (mode == XPATH_EVAL_ANY)
                breakPos = 1;
            else
                breakPos = mode;

            /*
            * Check if the node set contains a sufficient number of nodes for
            * the requested pos.
            */
            if (numNodes < breakPos)
                goto done;
        }
    }

    pos = 1;

    for (; (i >= 0) && (i < numNodes); i += incr) {
        xmlNodePtr node = nodes[i];
        int res;

        if (ctxt->pctxt.error != XPATH_EXPRESSION_OK) {
            res = 0;
        } else {
            xmlXPathItem item;

            ctxt->node = node;
            ctxt->proximityPosition = i + 1;

            if (xmlXPathCompOpEval(ctxt, &item, op->ch2,
                                   XPATH_EVAL_ANY) < 0) {
                res = -1;
            } else if (item.type == XPATH_NUMBER) {
                res = (item.as.number == ctxt->proximityPosition);
            } else {
                res = xmlXPathItemToBoolean(ctxt, &item);
            }
        }

        if (res > 0) {
            if (pos >= breakPos) {
                if (breakPos != 0) {
                    hit = node;
                    i += incr;
                    break;
                }

                if (i != j) {
                    nodes[j] = node;
                    nodes[i] = NULL;
                }

                j += 1;
            }

            pos += 1;
        } else {
            /* Remove the entry from the initial node set. */
            nodes[i] = NULL;
            if (node->type == XML_NAMESPACE_DECL)
                xmlXPathNodeSetFreeNs((xmlNsPtr) node);
        }
    }

done:
    /* Free remaining nodes. */
    if (hasNsNodes) {
        for (; (i >= 0) && (i < numNodes); i += incr) {
            xmlNodePtr node = nodes[i];
            if ((node != NULL) && (node->type == XML_NAMESPACE_DECL))
                xmlXPathNodeSetFreeNs((xmlNsPtr) node);
        }
    }

    if (breakPos != 0) {
        if (hit != NULL) {
            set->nodeTab[offset] = hit;
            set->nodeNr = offset + 1;
        } else {
            set->nodeNr = offset;
        }
    } else {
        set->nodeNr = offset + j;
    }

    ctxt->node = oldnode;
    ctxt->contextSize = oldcs;
    ctxt->proximityPosition = oldpp;
}

/**
 * xmlXPathCompOpEvalPredicate:
 * @ctxt:  the XPath Parser context
 * @opIndex:  the predicate op
 * @set:  the node set to filter
 * @offset:  filter nodes starting at offet
 * @mode:  evaluation mode or XPATH_EVAL_DEFAULT
 * @hasNsNodes:  true if the node set may contain namespace nodes
 *
 * Filter a node set, keeping only nodes for which the sequence of predicate
 * expressions matches. Afterwards, keep only nodes between minPos and maxPos
 * in the filtered result.
 */
static void
xmlXPathCompOpEvalPredicate(xmlXPathContextPtr ctxt, int opIndex,
			    xmlNodeSetPtr set, int offset,
                            xmlXPathEvalMode mode, int hasNsNodes) {
    const xmlXPathOp *steps = ctxt->pctxt.comp->steps;
    const xmlXPathOp *op = &steps[opIndex];

    if (mode == XPATH_EVAL_DEFAULT)
        mode = op->mode;

    if (op->ch1 != -1) {
	/*
	* Process inner predicates first.
	*/
	xmlXPathCompOpEvalPredicate(ctxt, op->ch1, set, offset,
                                    XPATH_EVAL_DEFAULT, hasNsNodes);

        if (ctxt->pctxt.error)
            return;
    }

    xmlXPathNodeSetFilter(ctxt, op, set, offset, mode, hasNsNodes);
}

/**
 * xmlXPathCompOpEvalStep:
 * @ctxt:  parser context
 * @obj:  the input nodeset object
 * @op:  step operation
 * @mode:  evaluation mode or XPATH_EVAL_DEFAULT
 *
 * Evaluate a step operation, taking the evaluation modes into
 * account.
 *
 * Returns the result of the step.
 */
ATTRIBUTE_NO_INLINE
static int
xmlXPathCompOpEvalStep(xmlXPathContextPtr ctxt, xmlXPathItem *result,
                       xmlNodePtr *inputNodes, int inputSize,
                       const xmlXPathOp *op, xmlXPathEvalMode mode) {
    int typeMask = op->as.step.typeMask;
    const xmlChar *name = op->qname.name;
    const xmlChar *URI = NULL;

    xmlNodeSetPtr outSeq;
    int inputIdx;
    int outOffset;
    xmlXPathEvalMode stepMode, predMode;
    int breakPos; /* The requested position() (when a "[n]" predicate) */
    int reverse;
    int duplMaxSize;

    if (inputSize <= 0)
        return(0);

    if ((op->mode == XPATH_EVAL_NONE) ||
        (op->predMode == XPATH_EVAL_NONE))
        goto done;

    /*
    * Set up namespaces
    */
    if (name != NULL) {
        if (ctxt->pctxt.comp->flags & XML_XPATH_COMPILE_NS) {
            URI = op->qname.ns.uri;
        } else if (op->qname.ns.prefix != NULL) {
            URI = xmlXPathNsLookup(ctxt, op->qname.ns.prefix);
            if (URI == NULL) {
                xmlXPathCErr(ctxt, XPATH_UNDEF_PREFIX_ERROR);
                goto done;
            }
        }
    }

    /*
     * Set up evaluation mode
     */

    reverse = 0;

    if (mode != XPATH_EVAL_DEFAULT)
        stepMode = mode;
    else
        stepMode = op->mode;

    if ((mode != XPATH_EVAL_DEFAULT) && (op->ch2 == -1))
        predMode = mode;
    else
        predMode = op->predMode;

    if (predMode == XPATH_EVAL_ALL) {
        breakPos = 0;
    } else {
        breakPos = 1;

        if (predMode == XPATH_EVAL_LAST) {
            reverse = 1;
        } else if ((predMode != XPATH_EVAL_ANY) &&
                   (predMode != XPATH_EVAL_NONE)) {
            breakPos = predMode;
        }
    }

    /*
     * Deduplication
     */
    if ((op->as.step.axis == AXIS_CHILD) ||
        (op->as.step.axis == AXIS_SELF) ||
        (op->as.step.axis == AXIS_ATTRIBUTE) ||
        (op->as.step.axis == AXIS_NAMESPACE)) {
        /*
         * These axes can't produce duplicates.
         */
        duplMaxSize = INT_MAX;
    } else {
        /*
         * Allow twice the number of input nodes before removing
         * duplicates. We have to loop over them anyway, so this
         * stays linear.
         */
        duplMaxSize = inputSize * 2;
    }

    /*
     * Loop over input nodes
     */
    inputIdx = 0;
    outSeq = &result->as.nodeset;
    outOffset = 0;
    while (inputIdx < inputSize) {
        xmlNodePtr start, cur;
        xmlIter iter;
        xmlIterNextFunc next;
        int pos, hasNsNodes;

	/*
	* Traverse the axis and test the nodes.
	*/

        start = inputNodes[inputIdx];
        cur = xmlIterStart[op->as.step.axis](&iter, start, reverse, &next);
	pos = 0;
	hasNsNodes = 0;

        while (cur != NULL) {
            xmlNodePtr add;

            if (OP_LIMIT_EXCEEDED(ctxt, 1))
                break;

            /*
             * Type test
             */
            if (((1 << cur->type) & typeMask) == 0)
                goto no_match;

            /*
             * Name test
             */
            if (name != NULL) {
                if (cur->type == XML_NAMESPACE_DECL) {
                    xmlNsPtr ns = (xmlNsPtr) cur;

                    if ((ns->prefix == NULL) ||
                        (strcmp((char *) name, (char *) ns->prefix) != 0))
                        goto no_match;

                    if (URI != NULL)
                        goto no_match;
                } else {
                    if ((name[0] != '*') &&
                        ((cur->name == NULL) ||
                         (strcmp((char *) name, (char *) cur->name) != 0)))
                        goto no_match;

                    if (URI != NULL) {
                        if ((cur->ns == NULL) ||
                            (strcmp((char *) URI, (char *) cur->ns->href) != 0))
                            goto no_match;
                    } else {
                        if (cur->ns != NULL)
                            goto no_match;
                    }
                }
            }

            if (++pos < breakPos)
                goto no_match;

            if (outSeq->nodeNr >= outSeq->nodeMax) {
                if (xmlXPathCacheNodeSetGrow(ctxt, outSeq) < 0)
                    break;
            }

            if (cur->type == XML_NAMESPACE_DECL) {
                xmlNodePtr parent = start;

                /*
                 * Namespace nodes can only be reached via the
                 * namespace axis starting with an element, or
                 * via the self axis starting with a namespace node.
                 */
                if (start->type == XML_NAMESPACE_DECL) {
                    xmlNsPtr ns = (xmlNsPtr) start;

                    parent = (xmlNodePtr) ns->next;
                }

                add = xmlXPathNodeSetDupNs(parent, (xmlNsPtr) cur);
                if (add == NULL) {
                    xmlXPathErrMemory(ctxt);
                    break;
                }
                hasNsNodes = 1;
                result->hasNsNodes = 1;
            } else {
                add = cur;
            }

            outSeq->nodeTab[outSeq->nodeNr++] = add;

	    if (breakPos != 0)
                break;

no_match:
            cur = next(&iter, cur);
        }

        if (iter.hasNodes) {
            if (iter.as.nodes == NULL)
                xmlXPathErrMemory(ctxt);
            else
                xmlFree(iter.as.nodes);
        }

        inputIdx++;

        if (outSeq->nodeNr <= outOffset)
            goto empty_set;

        /*
	* Apply predicates
	*/
        if (op->ch2 != -1) {
            xmlXPathCompOpEvalPredicate(ctxt, op->ch2, outSeq, outOffset,
                                        mode, hasNsNodes);

            if (outSeq->nodeNr <= outOffset)
                goto empty_set;
        }

        if (stepMode == XPATH_EVAL_ANY)
            break;

        if (outOffset > 0) {
            if ((stepMode == XPATH_EVAL_FIRST) ||
                (stepMode == XPATH_EVAL_LAST)) {
                xmlNodePtr node1, node2;
                int swap;

                /*
                 * Keep first or last
                 */

                node1 = outSeq->nodeTab[0];
                node2 = outSeq->nodeTab[1];
                swap = ((stepMode == XPATH_EVAL_FIRST) ? -1 : 1);

                if (
#ifdef XP_OPTIMIZED_NON_ELEM_COMPARISON
                     (xmlXPathCmpNodesExt(node1, node2))
#else
                     (xmlXPathCmpNodes(node1, node2))
#endif
                     == swap) {
                    outSeq->nodeTab[0] = node2;
                    node2 = node1;
                }

                outSeq->nodeNr = 1;
                if (node2->type == XML_NAMESPACE_DECL)
                    xmlFreeNs((xmlNsPtr) node2);
            } else {
                int numNodes = outSeq->nodeNr - outOffset;

                /*
                 * Only remove duplicates if the result grows twice as large
                 * as the largest set to merge.
                 */
                if (numNodes * 2 > duplMaxSize)
                    duplMaxSize = numNodes * 2;

                /*
                 * Deduplicate result dynamically
                 *
                 * Note that we currently allow duplicate nodes being returned
                 * from step and union operations which can cause unnecessary
                 * work for later operations. But duplicates should be rare
                 * and are limited to less than 50% of a nodeset.
                 */
                if (outSeq->nodeNr >= duplMaxSize) {
                    /*
                     * Note that we only sort to remove duplicates in
                     * xmlXPathNodeSetFinish.
                     */
                    xmlXPathNodeSetSort(outSeq);
                    xmlXPathNodeSetFinish(result, XPATH_EVAL_ALL);

                    /*
                     * Doubling the threshold should result in amortized
                     * O(n*log(n)) for all sort operations if there are no
                     * duplicates.
                     *
                     * If there are duplicates, a step can take quadratic
                     * time no matter what.
                     */
                    if (outSeq->nodeNr * 2 > duplMaxSize)
                        duplMaxSize = outSeq->nodeNr * 2;
                }
            }
        }

        outOffset = outSeq->nodeNr;

empty_set:
        if (ctxt->pctxt.error != XPATH_EXPRESSION_OK)
            break;
    }

done:
    if (ctxt->pctxt.error)
        return(-1);

    return(0);
}

ATTRIBUTE_NO_INLINE
static int
xmlXPathCompOpEvalFunc(xmlXPathContextPtr ctxt, const xmlXPathOp *op) {
    xmlXPathCompExprPtr comp = ctxt->pctxt.comp;
    xmlXPathFunction func = NULL;
    const xmlChar *oldFunc, *oldFuncURI;
    int flags = comp->flags;

    if (op->as.func != NULL) {
        func = op->as.func;
    } else {
        const xmlChar *URI = NULL;

        if (flags & XML_XPATH_COMPILE_NS) {
            URI = op->qname.ns.uri;
        } else if (op->qname.ns.prefix != NULL) {
            URI = xmlXPathNsLookup(ctxt, op->qname.ns.prefix);
            if (URI == NULL) {
                xmlXPathCErr(ctxt, XPATH_UNDEF_PREFIX_ERROR);
                return(-1);
            }
        }
        func = xmlXPathFunctionLookupNS(ctxt, op->qname.name,
                                        URI);
        if (func == NULL) {
            xmlXPathCErr(ctxt, XPATH_UNKNOWN_FUNC_ERROR);
            return(-1);
        }

        /*
         * This modifies the compiled expression and isn't
         * thread-safe.
         */
        if (func != NULL) {
            xmlXPathOpPtr mutOp = (xmlXPathOpPtr) op;

            if ((comp->dict == NULL) &&
                ((flags & XML_XPATH_COMPILE_NS) == 0) &&
                (mutOp->qname.ns.prefix != NULL)) {
                xmlFree(mutOp->qname.ns.prefix);
            }
            mutOp->qname.ns.uri = URI;
            mutOp->as.func = func;
         }
    }

    /*
     * In libxslt and possibly other applications, extension
     * functions can evaluate XPath expressions recursively,
     * reusing the XPath and parser contexts.
     *
     * This requires to back up and restore some state.
     *
     * TODO: We should think about backing up and restoring
     * the context node, size and position here. This has been
     * an endless source of bugs in libxslt.
     */

    oldFunc = ctxt->function;
    oldFuncURI = ctxt->functionURI;

    ctxt->function = op->qname.name;
    ctxt->functionURI = op->qname.ns.uri;

    func(&ctxt->pctxt, op->nbArgs);

    ctxt->function = oldFunc;
    ctxt->functionURI = oldFuncURI;

    if (ctxt->pctxt.error)
        return(-1);

    return(0);
}

ATTRIBUTE_NO_INLINE
static void
xmlXPathCompOpEvalSort(xmlXPathItem *item, const xmlXPathOp *op,
                       xmlXPathEvalMode mode) {
    xmlXPathEvalMode opMode;

    if (mode == XPATH_EVAL_DEFAULT)
        opMode = op->mode;
    else
        opMode = mode;

    switch (opMode) {
        case XPATH_EVAL_NONE:
            xmlXPathNodeSetClear(&item->as.nodeset, item->hasNsNodes);
            break;

        case XPATH_EVAL_ALL:
            if (item->as.nodeset.nodeNr > 1) {
                xmlXPathNodeSetSort(&item->as.nodeset);
                xmlXPathNodeSetFinish(item, opMode);
            }
            break;

        case XPATH_EVAL_ANY:
            break;

        case XPATH_EVAL_FIRST:
        case XPATH_EVAL_LAST:
            if (item->as.nodeset.nodeNr > 1)
                xmlXPathNodeSetFindFirst(item, opMode);
            break;

        default:
            if (item->as.nodeset.nodeNr > 1)
                xmlXPathNodeSetSort(&item->as.nodeset);
            xmlXPathNodeSetFinish(item, opMode);
            break;
    }
}

#ifdef XPATH_STATS

static int opCounts[XPATH_OP_TRUE];

ATTRIBUTE_DESTRUCTOR
static void
printCounts(void) {
    int total = 0;
    int i;

    for (i = 0; i < XPATH_OP_TRUE; i++) {
        printf("%2d: %d\n", i, opCounts[i]);
        total += opCounts[i];
    }

    printf("total: %d\n", total);
}

#endif

/**
 * xmlXPathCompOpEval:
 * @ctxt:  the XPath parser context with the compiled expression
 * @opIndex:  an XPath compiled operation
 * @mode:  evaluation mode or XPATH_EVAL_DEFAULT
 *
 * Evaluate a precompiled XPath operation. This is the main
 * interpreter loop.
 *
 * Returns the resulting object or NULL in case of error.
 */
static int
xmlXPathCompOpEval(xmlXPathContextPtr ctxt, xmlXPathItem *result,
                   int opIndex, xmlXPathEvalMode mode) {
    const xmlXPathOp *op;

    if (OP_LIMIT_EXCEEDED(ctxt, 1))
        return(-1);

    op = &ctxt->pctxt.comp->steps[opIndex];

#ifdef XPATH_STATS
    opCounts[op->op] += 1;
#endif

    switch (op->op) {
        case XPATH_OP_BOOL:
        case XPATH_OP_NOT: {
            int boolean;

            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            boolean = xmlXPathItemToBoolean(ctxt, result);
            if (op->op == XPATH_OP_NOT)
                boolean = !boolean;

            result->type = XPATH_BOOLEAN;
            result->as.boolean = boolean;
            break;
        }

        case XPATH_OP_NUMBER: {
            double number;

            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            number = xmlXPathItemToNumber(ctxt, result);
            result->type = XPATH_NUMBER;
            result->as.number = number;
            break;
        }

        case XPATH_OP_STRING: {
            xmlXPathItem arg;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            xmlXPathItemToString(ctxt, result, &arg);
            break;
        }

        case XPATH_OP_NODESET:
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (result->type != XPATH_NODESET) {
                xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
                break;
            }

            xmlXPathCompOpEvalNodeset(ctxt, result, op, mode);
            break;

        case XPATH_OP_XSLT_TREE:
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if ((result->type != XPATH_NODESET) &&
                (result->type != XPATH_XSLT_TREE))
                xmlXPathCErr(ctxt, XPATH_INVALID_TYPE);
            break;

        case XPATH_OP_AND:
        case XPATH_OP_OR: {
            int breakVal;

            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            breakVal = (op->op == XPATH_OP_OR);
            if (result->as.boolean == breakVal)
                break;

            xmlXPathCompOpEval(ctxt, result, op->ch2, XPATH_EVAL_DEFAULT);
            break;
        }

        case XPATH_OP_EQ:
        case XPATH_OP_NE: {
            xmlXPathItem arg;
            int ret;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;
            if (xmlXPathCompOpEval(ctxt, result, op->ch2,
                                   XPATH_EVAL_DEFAULT) < 0) {
                xmlXPathItemRelease(ctxt, &arg);
                break;
            }

	    ret = xmlXPathEqualValuesInternal(ctxt, &arg, result,
                                              (op->op == XPATH_OP_NE));

            result->type = XPATH_BOOLEAN;
            result->as.boolean = ret;
            break;
        }

        case XPATH_OP_LT:
        case XPATH_OP_LE: {
            xmlXPathItem arg;
            int ret;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;
            if (xmlXPathCompOpEval(ctxt, result, op->ch2,
                                   XPATH_EVAL_DEFAULT) < 0) {
                xmlXPathItemRelease(ctxt, &arg);
                break;
            }

            ret = xmlXPathCompareValuesInternal(ctxt, &arg, result,
                                                1, (op->op == XPATH_OP_LT));

            result->type = XPATH_BOOLEAN;
            result->as.boolean = ret;
            break;
        }

        case XPATH_OP_NEG:
        case XPATH_OP_FLOOR:
        case XPATH_OP_CEIL:
        case XPATH_OP_ROUND:
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            switch (op->op) {
                case XPATH_OP_NEG:
                    result->as.number = -result->as.number;
                    break;
                case XPATH_OP_FLOOR:
                    result->as.number = floor(result->as.number);
                    break;
                case XPATH_OP_CEIL:
                    result->as.number = ceil(result->as.number);
                    break;
                case XPATH_OP_ROUND:
                    result->as.number = xmlXPathRound(result->as.number);
                    break;
                default:
                    break;
            }

            break;

        case XPATH_OP_ADD:
        case XPATH_OP_SUB:
        case XPATH_OP_MULT:
        case XPATH_OP_DIV:
        case XPATH_OP_MOD: {
            xmlXPathItem arg;
            double val2;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch2,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            val2 = arg.as.number;

            switch (op->op) {
                case XPATH_OP_ADD:
                    result->as.number += val2;
                    break;
                case XPATH_OP_SUB:
                    result->as.number -= val2;
                    break;
                case XPATH_OP_MULT:
                    result->as.number *= val2;
                    break;
                case XPATH_OP_DIV:
                    result->as.number /= val2;
                    break;
                case XPATH_OP_MOD:
                    result->as.number = fmod(result->as.number, val2);
                    break;
                default:
                    break;
            }

            break;
        }

        case XPATH_OP_EQ_NUM:
        case XPATH_OP_NE_NUM:
        case XPATH_OP_LT_NUM:
        case XPATH_OP_LE_NUM: {
            xmlXPathItem arg;
            double val1, val2;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch2,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            val1 = result->as.number;
            val2 = arg.as.number;

            result->type = XPATH_BOOLEAN;

            switch (op->op) {
                case XPATH_OP_EQ_NUM:
                    result->as.boolean = (val1 == val2);
                    break;
                case XPATH_OP_NE_NUM:
                    result->as.boolean = (val1 != val2);
                    break;
                case XPATH_OP_LT_NUM:
                    result->as.boolean = (val1 < val2);
                    break;
                case XPATH_OP_LE_NUM:
                    result->as.boolean = (val1 <= val2);
                    break;
                default:
                    break;
            }

            break;
        }

        case XPATH_OP_EQ_STR:
        case XPATH_OP_NE_STR: {
            xmlXPathItem arg;
            const xmlChar *val1, *val2;
            int ret;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch2,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            val1 = result->as.string;
            val2 = arg.as.string;
            ret = (strcmp((char *) val1, (char *) val2) == 0);
            if (op->op == XPATH_OP_NE_STR)
                ret = !ret;

            xmlXPathItemReleaseString(result);
            xmlXPathItemReleaseString(&arg);

            result->type = XPATH_BOOLEAN;
            result->as.boolean = ret;

            break;
        }

        case XPATH_OP_UNION: {
            xmlXPathItem arg;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch2,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (((mode == XPATH_EVAL_ANY) || (op->mode == XPATH_EVAL_ANY)) &&
                 (arg.as.nodeset.nodeNr > 0)) {
                *result = arg;
                break;
            }

            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0) {
                xmlXPathItemReleaseNodeSet(ctxt, &arg);
                break;
            }

            if ((ctxt->opLimit != 0) &&
                ((xmlXPathCheckOpLimit(ctxt,
                                       result->as.nodeset.nodeNr) < 0) ||
                 (xmlXPathCheckOpLimit(ctxt,
                                       arg.as.nodeset.nodeNr) < 0))) {
                xmlXPathItemReleaseNodeSet(ctxt, &arg);
                xmlXPathItemReleaseNodeSet(ctxt, result);
                break;
            }

            if (mode == XPATH_EVAL_DEFAULT)
                mode = op->mode;

	    if ((mode != XPATH_EVAL_ANY) &&
                (arg.as.nodeset.nodeNr > 0)) {
                if (result->as.nodeset.nodeNr <= 0) {
                    xmlXPathItemReleaseNodeSet(ctxt, result);
                    *result = arg;
                    break;
                } else {
                    if (xmlXPathNodeSetMergeAndClear(ctxt, result, &arg,
                                                     mode) < 0) {
                        xmlXPathItemReleaseNodeSet(ctxt, result);
                        xmlXPathErrMemory(ctxt);
                    }
                }
	    }

            xmlXPathItemReleaseNodeSet(ctxt, &arg);
            break;
        }

        case XPATH_OP_ROOT: {
            xmlDocPtr doc;

            if (xmlXPathItemInitNodeSet(ctxt, result) < 0)
                break;

            doc = xmlXPathGetRoot(ctxt);
            if (doc != NULL) {
                result->as.nodeset.nodeTab[0] = (xmlNodePtr) doc;
                result->as.nodeset.nodeNr = 1;
            }
            break;
        }

        case XPATH_OP_NODE: {
            xmlNodePtr node;

            if (xmlXPathItemInitNodeSet(ctxt, result) < 0)
                break;

            node = ctxt->node;
            if (node != NULL) {
                if (node->type == XML_NAMESPACE_DECL) {
                    xmlNsPtr ns = (xmlNsPtr) node;

                    node = xmlXPathNodeSetDupNs((xmlNodePtr) ns->next, ns);
                    if (node == NULL) {
                        xmlXPathErrMemory(ctxt);
                        break;
                    }

                    result->hasNsNodes = 1;
                }

                result->as.nodeset.nodeTab[0] = node;
                result->as.nodeset.nodeNr = 1;
            }
            break;
        }

        case XPATH_OP_STEP: {
            xmlXPathItem arg;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (xmlXPathItemInitNodeSet(ctxt, result) < 0) {
                xmlXPathItemReleaseNodeSet(ctxt, &arg);
                break;
            }

            if (xmlXPathCompOpEvalStep(ctxt, result, arg.as.nodeset.nodeTab,
                                       arg.as.nodeset.nodeNr, op, mode) < 0)
                xmlXPathItemReleaseNodeSet(ctxt, result);

            xmlXPathItemReleaseNodeSet(ctxt, &arg);
            break;
        }

        case XPATH_OP_STEP_CTXT:
            if (xmlXPathItemInitNodeSet(ctxt, result) < 0)
                break;

            if (ctxt->node != NULL) {
                if (xmlXPathCompOpEvalStep(ctxt, result, &ctxt->node, 1,
                                           op, mode) < 0)
                    xmlXPathItemReleaseNodeSet(ctxt, result);
            }
            break;

        case XPATH_OP_VALUE_BOOL:
            result->type = XPATH_BOOLEAN;
            result->as.boolean = op->as.boolean;
            break;

        case XPATH_OP_VALUE_NUMBER:
            result->type = XPATH_NUMBER;
            result->as.number = op->as.number;
            break;

        case XPATH_OP_VALUE_STRING:
            result->type = XPATH_STRING;
            result->isCopy = 1;
            result->as.string = op->as.string;
            break;

        case XPATH_OP_VARIABLE: {
            xmlXPathObjectPtr obj = NULL;
            const xmlChar *URI = NULL;
            int copy = 0;

            if (ctxt->pctxt.comp->flags & XML_XPATH_COMPILE_NS) {
                URI = op->qname.ns.uri;
            } else if (op->qname.ns.prefix != NULL) {
                URI = xmlXPathNsLookup(ctxt, op->qname.ns.prefix);
                if (URI == NULL) {
                    xmlXPathCErr(ctxt, XPATH_UNDEF_PREFIX_ERROR);
                    break;
                }
            }

            if (ctxt->varLookupFunc != NULL)
                obj = ctxt->varLookupFunc(ctxt->varLookupData,
                                               op->qname.name, URI);

            if ((obj == NULL) && (ctxt->varHash != NULL)) {
                obj = xmlHashLookup2(ctxt->varHash, op->qname.name, URI);
                copy = 1;
            }

            if (obj == NULL) {
                xmlXPathCErr(ctxt, XPATH_UNDEF_VARIABLE_ERROR);
                break;
            }

            xmlXPathItemFromObj(ctxt, result, obj, copy);
            break;
        }

        case XPATH_OP_SFUNC: {
            xmlXPathObjectPtr obj;
            int frame;

            frame = ctxt->pctxt.valueNr;
            if (op->ch2 != -1) {
                /* next arg */
                if (xmlXPathCompOpEval(ctxt, result, op->ch2,
                                       XPATH_EVAL_DEFAULT) < 0)
                    goto sfunc_cleanup;
            }

            if (op->ch1 != -1) {
                /* last arg */
                if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                       XPATH_EVAL_DEFAULT) < 0)
                    goto sfunc_cleanup;

                obj = xmlXPathItemToObj(ctxt, result);
                if (obj == NULL)
                    goto sfunc_cleanup;

                xmlXPathValuePushInternal(ctxt, obj);
            }

            op->as.func(&ctxt->pctxt, op->nbArgs);

            if (ctxt->pctxt.error)
                goto sfunc_cleanup;

            obj = xmlXPathValuePopInternal(ctxt);
            if (obj == NULL)
                break;

            xmlXPathItemFromObj(ctxt, result, obj, 0);
            break;

sfunc_cleanup:
            while (ctxt->pctxt.valueNr > frame)
                xmlXPathReleaseObject(ctxt, xmlXPathValuePopInternal(ctxt));
            break;
        }

        case XPATH_OP_FUNCTION: {
            xmlXPathObjectPtr obj;
            int frame;

            frame = ctxt->pctxt.valueNr;
            if (op->ch2 != -1) {
                /* next arg */
                if (xmlXPathCompOpEval(ctxt, result, op->ch2,
                                       XPATH_EVAL_DEFAULT) < 0)
                    goto func_cleanup;
            }

            if (op->ch1 != -1) {
                /* last arg */
                if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                       XPATH_EVAL_DEFAULT) < 0)
                    goto func_cleanup;

                obj = xmlXPathItemToObj(ctxt, result);
                if (obj == NULL)
                    goto func_cleanup;

                xmlXPathValuePushInternal(ctxt, obj);
            }

            if (xmlXPathCompOpEvalFunc(ctxt, op) < 0)
                goto func_cleanup;

            if ((ctxt->pctxt.error == XPATH_EXPRESSION_OK) &&
                (ctxt->pctxt.valueNr != frame + 1)) {
                xmlXPathCErr(ctxt, XPATH_STACK_ERROR);
            } else {
                obj = xmlXPathValuePopInternal(ctxt);

                xmlXPathItemFromObj(ctxt, result, obj, 0);
            }

func_cleanup:
            while (ctxt->pctxt.valueNr > frame)
                xmlXPathReleaseObject(ctxt,
                                      xmlXPathValuePopInternal(ctxt));
            break;
        }

        case XPATH_OP_ARG: {
            xmlXPathObjectPtr obj;

            if (op->ch2 != -1) {
                /* next arg */
                if (xmlXPathCompOpEval(ctxt, result, op->ch2,
                                       XPATH_EVAL_DEFAULT) < 0)
                    break;
            }
            /* arg */
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            obj = xmlXPathItemToObj(ctxt, result);
            if (obj == NULL)
                break;

            xmlXPathValuePushInternal(ctxt, obj);
            break;
        }

        case XPATH_OP_FILTER:
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (result->isCopy) {
                if (xmlXPathItemCloneNodeSet(ctxt, result) < 0)
                    break;
            }

            xmlXPathNodeSetFilter(ctxt, op, &result->as.nodeset, 0, mode, 1);
            if (ctxt->pctxt.error)
                xmlXPathItemReleaseNodeSet(ctxt, result);
            break;

        case XPATH_OP_SORT:
            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (result->type == XPATH_NODESET)
                xmlXPathCompOpEvalSort(result, op, mode);
            break;

        case XPATH_OP_LAST: {
            int contextSize = ctxt->contextSize;

            if (contextSize < 0) {
                xmlXPathCErr(ctxt, XPATH_INVALID_CTXT_SIZE);
                break;
            }

            result->type = XPATH_NUMBER;
            result->as.number = contextSize;
            break;
        }

        case XPATH_OP_POSITION: {
            int position = ctxt->proximityPosition;

            if (position < 0) {
                xmlXPathCErr(ctxt, XPATH_INVALID_CTXT_SIZE);
                break;
            }

            result->type = XPATH_NUMBER;
            result->as.number = position;
            break;
        }

        case XPATH_OP_LOCAL_NAME: {
            const xmlChar *name;

            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (result->as.nodeset.nodeNr <= 0)
                name = BAD_CAST "";
            else
                name = xmlXPathLocalName(result->as.nodeset.nodeTab[0]);

            xmlXPathItemReleaseNodeSet(ctxt, result);

            result->type = XPATH_STRING;
            result->isCopy = 1;
            result->as.string = (xmlChar *) name;
            break;
        }

        case XPATH_OP_LOCAL_NAME_CTXT: {
            const xmlChar *name;

            name = xmlXPathLocalName(ctxt->node);

            result->type = XPATH_STRING;
            result->isCopy = 1;
            result->as.string = (xmlChar *) name;
            break;
        }

        case XPATH_OP_NAME: {
            xmlXPathItem arg;

            if (xmlXPathCompOpEval(ctxt, &arg, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (arg.as.nodeset.nodeNr <= 0) {
                result->type = XPATH_STRING;
                result->isCopy = 1;
                result->as.string = BAD_CAST "";
            } else {
                xmlXPathName(ctxt, result, arg.as.nodeset.nodeTab[0]);
            }

            xmlXPathItemReleaseNodeSet(ctxt, &arg);
            break;
        }

        case XPATH_OP_NAME_CTXT:
            xmlXPathName(ctxt, result, ctxt->node);
            break;

        case XPATH_OP_NAMESPACE_URI: {
            const xmlChar *uri;

            if (xmlXPathCompOpEval(ctxt, result, op->ch1,
                                   XPATH_EVAL_DEFAULT) < 0)
                break;

            if (result->as.nodeset.nodeNr <= 0)
                uri = BAD_CAST "";
            else
                uri = xmlXPathNamespaceUri(result->as.nodeset.nodeTab[0]);

            xmlXPathItemReleaseNodeSet(ctxt, result);

            result->type = XPATH_STRING;
            result->isCopy = 1;
            result->as.string = (xmlChar *) uri;
            break;
        }

        case XPATH_OP_NAMESPACE_URI_CTXT: {
            const xmlChar *uri;

            uri = xmlXPathNamespaceUri(ctxt->node);

            result->type = XPATH_STRING;
            result->isCopy = 1;
            result->as.string = (xmlChar *) uri;
            break;
        }

        default:
            xmlXPathCErr(ctxt, XPATH_INVALID_OPERAND);
            break;
    }

    if (ctxt->pctxt.error)
        return(-1);

    return(0);
}

#ifdef XPATH_STREAMING
/**
 * xmlXPathRunStreamEval:
 * @pctxt:  the XPath parser context with the compiled expression
 *
 * Evaluate the Precompiled Streamable XPath expression in the given context.
 */
static int
xmlXPathRunStreamEval(xmlXPathParserContextPtr pctxt, xmlPatternPtr comp,
		      xmlXPathObjectPtr *resultSeq, int toBool)
{
    int max_depth, min_depth;
    int from_root;
    int ret, depth;
    int eval_all_nodes;
    xmlNodePtr cur = NULL, limit = NULL;
    xmlDocPtr doc;
    xmlStreamCtxtPtr patstream = NULL;
    xmlXPathContextPtr ctxt = pctxt->context;

    if ((ctxt == NULL) || (comp == NULL))
        return(-1);
    max_depth = xmlPatternMaxDepth(comp);
    if (max_depth == -1)
        return(-1);
    if (max_depth == -2)
        max_depth = 10000;
    min_depth = xmlPatternMinDepth(comp);
    if (min_depth == -1)
        return(-1);
    from_root = xmlPatternFromRoot(comp);
    if (from_root < 0)
        return(-1);

    if (! toBool) {
	if (resultSeq == NULL)
	    return(-1);
	*resultSeq = xmlXPathCacheNewNodeSet(pctxt);
	if (*resultSeq == NULL)
	    return(-1);
    }

    doc = xmlXPathGetRoot(ctxt);

    /*
     * handle the special cases of "/" amd "." being matched
     */
    if (min_depth == 0) {
        int res;

	if (from_root) {
	    /* Select "/" */
	    if (toBool)
		return(1);
            res = xmlXPathNodeSetAddUnique((*resultSeq)->nodesetval,
                                           (xmlNodePtr) doc);
	} else {
	    /* Select "self::node()" */
	    if (toBool)
		return(1);
            res = xmlXPathNodeSetAddUnique((*resultSeq)->nodesetval,
                                           ctxt->node);
	}

        if (res < 0)
            xmlXPathErrMemory(ctxt);
    }
    if (max_depth == 0) {
	return(0);
    }

    if (from_root) {
        cur = (xmlNodePtr) doc;
    } else if (ctxt->node != NULL) {
        switch (ctxt->node->type) {
            case XML_ELEMENT_NODE:
            case XML_DOCUMENT_NODE:
            case XML_DOCUMENT_FRAG_NODE:
            case XML_HTML_DOCUMENT_NODE:
	        cur = ctxt->node;
		break;
            case XML_ATTRIBUTE_NODE:
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
            case XML_ENTITY_REF_NODE:
            case XML_ENTITY_NODE:
            case XML_PI_NODE:
            case XML_COMMENT_NODE:
            case XML_NOTATION_NODE:
            case XML_DTD_NODE:
            case XML_DOCUMENT_TYPE_NODE:
            case XML_ELEMENT_DECL:
            case XML_ATTRIBUTE_DECL:
            case XML_ENTITY_DECL:
            case XML_NAMESPACE_DECL:
            case XML_XINCLUDE_START:
            case XML_XINCLUDE_END:
		break;
	}
	limit = cur;
    }
    if (cur == NULL) {
        return(0);
    }

    patstream = xmlPatternGetStreamCtxt(comp);
    if (patstream == NULL) {
        xmlXPathErrMemory(ctxt);
	return(-1);
    }

    eval_all_nodes = xmlStreamWantsAnyNode(patstream);

    if (from_root) {
	ret = xmlStreamPush(patstream, NULL, NULL);
	if (ret < 0) {
	} else if (ret == 1) {
	    if (toBool)
		goto return_1;
	    if (xmlXPathNodeSetAddUnique((*resultSeq)->nodesetval, cur) < 0)
                xmlXPathErrMemory(ctxt);
	}
    }
    depth = 0;
    goto scan_children;
next_node:
    do {
        if (ctxt->opLimit != 0) {
            if (ctxt->opCount >= ctxt->opLimit) {
                xmlXPathCErr(ctxt, XPATH_RECURSION_LIMIT_EXCEEDED);
                xmlFreeStreamCtxt(patstream);
                return(-1);
            }
            ctxt->opCount++;
        }

	switch (cur->type) {
	    case XML_ELEMENT_NODE:
	    case XML_TEXT_NODE:
	    case XML_CDATA_SECTION_NODE:
	    case XML_COMMENT_NODE:
	    case XML_PI_NODE:
		if (cur->type == XML_ELEMENT_NODE) {
		    ret = xmlStreamPush(patstream, cur->name,
				(cur->ns ? cur->ns->href : NULL));
		} else if (eval_all_nodes)
		    ret = xmlStreamPushNode(patstream, NULL, NULL, cur->type);
		else
		    break;

		if (ret < 0) {
		    xmlXPathErrMemory(ctxt);
		} else if (ret == 1) {
		    if (toBool)
			goto return_1;
		    if (xmlXPathNodeSetAddUnique((*resultSeq)->nodesetval,
                                                 cur) < 0)
                        xmlXPathErrMemory(ctxt);
		}
		if ((cur->children == NULL) || (depth >= max_depth)) {
		    ret = xmlStreamPop(patstream);
		    while (cur->next != NULL) {
			cur = cur->next;
			if ((cur->type != XML_ENTITY_DECL) &&
			    (cur->type != XML_DTD_NODE))
			    goto next_node;
		    }
		}
	    default:
		break;
	}

scan_children:
	if (cur->type == XML_NAMESPACE_DECL) break;
	if ((cur->children != NULL) && (depth < max_depth)) {
	    /*
	     * Do not descend on entities declarations
	     */
	    if (cur->children->type != XML_ENTITY_DECL) {
		cur = cur->children;
		depth++;
		/*
		 * Skip DTDs
		 */
		if (cur->type != XML_DTD_NODE)
		    continue;
	    }
	}

	if (cur == limit)
	    break;

	while (cur->next != NULL) {
	    cur = cur->next;
	    if ((cur->type != XML_ENTITY_DECL) &&
		(cur->type != XML_DTD_NODE))
		goto next_node;
	}

	do {
	    cur = cur->parent;
	    depth--;
	    if ((cur == NULL) || (cur == limit) ||
                (cur->type == XML_DOCUMENT_NODE))
	        goto done;
	    if (cur->type == XML_ELEMENT_NODE) {
		ret = xmlStreamPop(patstream);
	    } else if ((eval_all_nodes) &&
		((cur->type == XML_TEXT_NODE) ||
		 (cur->type == XML_CDATA_SECTION_NODE) ||
		 (cur->type == XML_COMMENT_NODE) ||
		 (cur->type == XML_PI_NODE)))
	    {
		ret = xmlStreamPop(patstream);
	    }
	    if (cur->next != NULL) {
		cur = cur->next;
		break;
	    }
	} while (cur != NULL);

    } while ((cur != NULL) && (depth >= 0));

done:

    if (patstream)
	xmlFreeStreamCtxt(patstream);
    return(0);

return_1:
    if (patstream)
	xmlFreeStreamCtxt(patstream);
    return(1);
}
#endif /* XPATH_STREAMING */

/**
 * xmlXPathRunEval:
 * @ctxt:  the XPath parser context with the compiled expression
 * @toBool:  evaluate to a boolean result
 *
 * Evaluate the Precompiled XPath expression in the given context.
 */
static int
xmlXPathRunEval(xmlXPathContextPtr ctxt, xmlXPathObjectPtr *resObjPtr,
                int toBool)
{
    const xmlXPathCompExpr *comp = ctxt->pctxt.comp;
    int oldDepth;
    int res;

#ifdef XPATH_STREAMING
    if (comp->stream)
	return(xmlXPathRunStreamEval(&ctxt->pctxt, comp->stream, resObjPtr,
                                     toBool));
#endif

    if (comp->maxEvalDepth > XPATH_MAX_RECURSION_DEPTH - ctxt->depth) {
        xmlXPathCErr(ctxt, XPATH_RECURSION_LIMIT_EXCEEDED);
        return(-1);
    }

    oldDepth = ctxt->depth;
    ctxt->depth += comp->maxEvalDepth;

    if (toBool) {
        xmlXPathItem item;

        if (xmlXPathCompOpEval(ctxt, &item, comp->root, XPATH_EVAL_ANY) < 0)
            res = -1;
        else
            res = xmlXPathItemToBoolean(ctxt, &item);
    } else {
        xmlXPathItem item;

	if (xmlXPathCompOpEval(ctxt, &item, comp->root,
                               XPATH_EVAL_DEFAULT) < 0) {
            res = -1;
        } else {
            xmlXPathObjectPtr resObj;

            resObj = xmlXPathItemToObj(ctxt, &item);

            if (resObj != NULL) {
                *resObjPtr = resObj;
                res = 0;
            } else {
                res = -1;
            }
        }
    }

    ctxt->depth = oldDepth;

    return(res);
}

/************************************************************************
 *									*
 *			Public interfaces				*
 *									*
 ************************************************************************/

/**
 * xmlXPathEvalPredicate:
 * @ctxt:  the XPath context
 * @res:  the Predicate Expression evaluation result
 *
 * Evaluate a predicate result for the current node.
 * A PredicateExpr is evaluated by evaluating the Expr and converting
 * the result to a boolean. If the result is a number, the result will
 * be converted to true if the number is equal to the position of the
 * context node in the context node list (as returned by the position
 * function) and will be converted to false otherwise; if the result
 * is not a number, then the result will be converted as if by a call
 * to the boolean function.
 *
 * Returns 1 if predicate is true, 0 otherwise
 */
int
xmlXPathEvalPredicate(xmlXPathContextPtr ctxt, xmlXPathObjectPtr res) {
    if ((ctxt == NULL) || (res == NULL)) return(0);
    switch (res->type) {
        case XPATH_BOOLEAN:
	    return(res->boolval);
        case XPATH_NUMBER:
	    return(res->floatval == ctxt->proximityPosition);
        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
	    if (res->nodesetval == NULL)
		return(0);
	    return(res->nodesetval->nodeNr != 0);
        case XPATH_STRING:
	    return((res->stringval != NULL) &&
	           (xmlStrlen(res->stringval) != 0));
        default:
	    break;
    }
    return(0);
}

/**
 * xmlXPathEvaluatePredicateResult:
 * @ctxt:  the XPath Parser context
 * @res:  the Predicate Expression evaluation result
 *
 * DEPRECATED: Internal function, don't use.
 *
 * Evaluate a predicate result for the current node.
 * A PredicateExpr is evaluated by evaluating the Expr and converting
 * the result to a boolean. If the result is a number, the result will
 * be converted to true if the number is equal to the position of the
 * context node in the context node list (as returned by the position
 * function) and will be converted to false otherwise; if the result
 * is not a number, then the result will be converted as if by a call
 * to the boolean function.
 *
 * Returns 1 if predicate is true, 0 otherwise
 */
int
xmlXPathEvaluatePredicateResult(xmlXPathParserContextPtr ctxt,
                                xmlXPathObjectPtr res) {
    if ((ctxt == NULL) || (res == NULL)) return(0);
    switch (res->type) {
        case XPATH_BOOLEAN:
	    return(res->boolval);
        case XPATH_NUMBER:
#if defined(__BORLANDC__) || (defined(_MSC_VER) && (_MSC_VER == 1200))
	    return((res->floatval == ctxt->context->proximityPosition) &&
	           (!xmlXPathIsNaN(res->floatval))); /* MSC pbm Mark Vakoc !*/
#else
	    return(res->floatval == ctxt->context->proximityPosition);
#endif
        case XPATH_NODESET:
        case XPATH_XSLT_TREE:
	    if (res->nodesetval == NULL)
		return(0);
	    return(res->nodesetval->nodeNr != 0);
        case XPATH_STRING:
	    return((res->stringval != NULL) && (res->stringval[0] != 0));
        default:
	    break;
    }
    return(0);
}

#ifdef XPATH_STREAMING
/**
 * xmlXPathTryStreamCompile:
 * @ctxt: an XPath context
 * @str:  the XPath expression
 *
 * Try to compile the XPath expression as a streamable subset.
 *
 * Returns the compiled expression or NULL if failed to compile.
 */
static xmlXPathCompExprPtr
xmlXPathTryStreamCompile(xmlXPathContextPtr ctxt, const xmlChar *str) {
    /*
     * Optimization: use streaming patterns when the XPath expression can
     * be compiled to a stream lookup
     */
    xmlPatternPtr stream;
    xmlXPathCompExprPtr comp;
    xmlDictPtr dict = NULL;
    const xmlChar **namespaces = NULL;
    xmlNsPtr ns;
    int i, j;

    if ((!xmlStrchr(str, '[')) && (!xmlStrchr(str, '(')) &&
        (!xmlStrchr(str, '@'))) {
	const xmlChar *tmp;
        int res;

	/*
	 * We don't try to handle expressions using the verbose axis
	 * specifiers ("::"), just the simplified form at this point.
	 * Additionally, if there is no list of namespaces available and
	 *  there's a ":" in the expression, indicating a prefixed QName,
	 *  then we won't try to compile either. xmlPatterncompile() needs
	 *  to have a list of namespaces at compilation time in order to
	 *  compile prefixed name tests.
	 */
	tmp = xmlStrchr(str, ':');
	if ((tmp != NULL) &&
	    ((ctxt == NULL) || (ctxt->nsNr == 0) || (tmp[1] == ':')))
	    return(NULL);

	if (ctxt != NULL) {
	    dict = ctxt->dict;
	    if (ctxt->nsNr > 0) {
		namespaces = xmlMalloc(2 * (ctxt->nsNr + 1) * sizeof(xmlChar*));
		if (namespaces == NULL) {
		    xmlXPathErrMemory(ctxt);
		    return(NULL);
		}
		for (i = 0, j = 0; (j < ctxt->nsNr); j++) {
		    ns = ctxt->namespaces[j];
		    namespaces[i++] = ns->href;
		    namespaces[i++] = ns->prefix;
		}
		namespaces[i++] = NULL;
		namespaces[i] = NULL;
	    }
	}

	res = xmlPatternCompileSafe(str, dict, XML_PATTERN_XPATH, namespaces,
                                    &stream);
	if (namespaces != NULL) {
	    xmlFree((xmlChar **)namespaces);
	}
        if (res < 0) {
            xmlXPathErrMemory(ctxt);
            return(NULL);
        }
	if ((stream != NULL) && (xmlPatternStreamable(stream) == 1)) {
	    comp = xmlXPathNewCompExpr();
	    if (comp == NULL) {
		xmlXPathErrMemory(ctxt);
	        xmlFreePattern(stream);
		return(NULL);
	    }
	    comp->stream = stream;
	    comp->dict = dict;
	    if (comp->dict)
		xmlDictReference(comp->dict);
	    return(comp);
	}
	xmlFreePattern(stream);
    }
    return(NULL);
}
#endif /* XPATH_STREAMING */

typedef struct {
    int maxEvalDepth;
    int maxNodes;
} xmlXPathExprStats;

static int
xmlXPathOptimizeExpression(xmlXPathContextPtr ctxt, xmlXPathCompExprPtr comp,
                           int opIndex, xmlXPathExprStats *stats) {
    xmlXPathOpPtr op = &comp->steps[opIndex];
    int maxEvalDepth, maxNodes, argIndex;

    /*
    * Try to rewrite "descendant-or-self::node()/foo" to an optimized
    * internal representation.
    */

    if ((op->op == XPATH_OP_STEP) &&
        (op->ch1 != -1) &&
        (op->ch2 == -1 /* no predicate */) &&
        ((op->predMode == XPATH_EVAL_ALL) ||
         (op->predMode == XPATH_EVAL_ANY)))
    {
        xmlXPathOpPtr prevop = &comp->steps[op->ch1];

        if (((prevop->op == XPATH_OP_STEP) ||
             (prevop->op == XPATH_OP_STEP_CTXT)) &&
            (prevop->as.step.axis == AXIS_DESCENDANT_OR_SELF) &&
            (prevop->as.step.typeMask == TYPE_MASK_NODE) &&
            (prevop->ch2 == -1) &&
            ((prevop->predMode == XPATH_EVAL_ALL) ||
             (prevop->predMode == XPATH_EVAL_ANY)))
        {
            /*
            * This is a "descendant-or-self::node()" without predicates.
            * Try to eliminate it.
            */

            switch (op->as.step.axis) {
                case AXIS_CHILD:
                case AXIS_DESCENDANT:
                    /*
                    * Convert "descendant-or-self::node()/child::" or
                    * "descendant-or-self::node()/descendant::" to
                    * "descendant::"
                    */
                    op->op = prevop->op;
                    op->ch1 = prevop->ch1;
                    op->as.step.axis = AXIS_DESCENDANT;
                    break;
                case AXIS_SELF:
                case AXIS_DESCENDANT_OR_SELF:
                    /*
                    * Convert "descendant-or-self::node()/self::" or
                    * "descendant-or-self::node()/descendant-or-self::" to
                    * to "descendant-or-self::"
                    */
                    op->op = prevop->op;
                    op->ch1 = prevop->ch1;
                    op->as.step.axis = AXIS_DESCENDANT_OR_SELF;
                    break;
                default:
                    break;
            }
	}
    }

    /* Recurse */

    if (ctxt->depth >= XPATH_MAX_RECURSION_DEPTH) {
        xmlXPathCErr(ctxt, XPATH_RECURSION_LIMIT_EXCEEDED);
        return(-1);
    }


    maxEvalDepth = 0;
    maxNodes = INT_MAX;

    if ((op->op == XPATH_OP_NODE) ||
        (op->op == XPATH_OP_ROOT) ||
        (((op->op == XPATH_OP_STEP) ||
          (op->op == XPATH_OP_STEP_CTXT) ||
          (op->op == XPATH_OP_FILTER)) &&
         (op->mode != XPATH_EVAL_ALL)) ||
        (((op->op == XPATH_OP_NODESET) ||
          (op->op == XPATH_OP_SORT)) &&
         ((op->mode != XPATH_EVAL_ALL) &&
          (op->mode != XPATH_EVAL_ANY))))
        maxNodes = 1;

    if (op->ch1 != -1) {
        xmlXPathExprStats childStats;

        ctxt->depth += 1;
        argIndex = xmlXPathOptimizeExpression(ctxt, comp, op->ch1,
                                              &childStats);
        ctxt->depth -= 1;

        if (argIndex < 0)
            return(argIndex);

        op->ch1 = argIndex;
        maxEvalDepth = childStats.maxEvalDepth;

        if (op->op == XPATH_OP_UNION)
            maxNodes = childStats.maxNodes;

        /*
         * Eliminate OP_SORT on single nodes and with mode ANY
         */
        if ((op->op == XPATH_OP_SORT) &&
            ((op->mode == XPATH_EVAL_ANY) ||
             (childStats.maxNodes <= 1))) {
            *stats = childStats;
            return(op->ch1);
        }
    }

    if (op->ch2 != -1) {
        xmlXPathExprStats childStats;

        ctxt->depth += 1;
        argIndex = xmlXPathOptimizeExpression(ctxt, comp, op->ch2,
                                              &childStats);
        ctxt->depth -= 1;

        if (argIndex < 0)
            return(argIndex);

        op->ch2 = argIndex;

        if (op->op == XPATH_OP_PREDICATE) {
            /*
             * Stack usage of recursive callchain:
             *
             * Eval (112) ->
             * EvalStep (224) ->
             * EvalPredicate (80) ->
             * Filter (192) ->
             * Eval
             */
            childStats.maxEvalDepth += 5;
        } else if (op->op == XPATH_OP_FILTER) {
            /*
             * Eval (112) ->
             * Filter (192) ->
             * Eval
             */
            childStats.maxEvalDepth += 2;
        }

        if (childStats.maxEvalDepth > maxEvalDepth)
            maxEvalDepth = childStats.maxEvalDepth;

        if (op->op == XPATH_OP_UNION) {
            if ((op->mode == XPATH_EVAL_ANY) ||
                (op->mode == XPATH_EVAL_FIRST) ||
                (op->mode == XPATH_EVAL_LAST)) {
                if (childStats.maxNodes > maxNodes)
                    maxNodes = childStats.maxNodes;
            } else {
                maxNodes = INT_MAX;
            }
        }
    }

    stats->maxNodes = maxNodes;
    stats->maxEvalDepth = maxEvalDepth + 1;

    return(opIndex);
}

static int
xmlXPathDoCompile(xmlXPathContext *ctxt) {
    xmlXPathCompExprPtr comp;
    xmlXPathExprStats stats = { 0, 0 };
    int oldDepth, opIndex;
    int ret = -1;

#ifdef XPATH_STREAMING
    comp = xmlXPathTryStreamCompile(ctxt, ctxt->pctxt.base);
    if ((comp == NULL) &&
        (ctxt->lastError.code == XML_ERR_NO_MEMORY)) {
        xmlXPathErrMemory(ctxt);
        return(-1);
    }
    if (comp != NULL) {
        /* See XPointer comment below */
        if (ctxt->pctxt.comp != NULL)
	    xmlXPathFreeCompExpr(ctxt->pctxt.comp);
        ctxt->pctxt.comp = comp;
        return(0);
    }
#endif

    /*
     * The XPointer code can call xmlXPathEvalExpr multiple times
     * leading to a compiled expression still stored in the parser
     * context. This seems like a bug.
     */
    if (ctxt->pctxt.comp == NULL) {
        comp = xmlXPathNewCompExpr();
        if (comp == NULL) {
            xmlXPathErrMemory(ctxt);
            return(-1);
        }
        if (ctxt->dict != NULL) {
            comp->dict = ctxt->dict;
            xmlDictReference(comp->dict);
        }

        ctxt->pctxt.comp = comp;
    } else {
        comp = ctxt->pctxt.comp;
    }

    comp->flags = ctxt->flags;

    oldDepth = ctxt->depth;

    opIndex = xmlXPathCompileExpr(ctxt);
    if (opIndex < 0)
        goto error;

    opIndex = xmlXPathCompAddSort(ctxt, opIndex);
    if (opIndex < 0)
        goto error;

    if (*ctxt->pctxt.cur != 0) {
	xmlXPathCErr(ctxt, XPATH_EXPR_ERROR);
        goto error;
    }

    opIndex = xmlXPathOptimizeExpression(ctxt, comp, opIndex, &stats);
    if (opIndex < 0)
        goto error;

    if (stats.maxEvalDepth >= XPATH_MAX_RECURSION_DEPTH) {
        xmlXPathCErr(ctxt, XPATH_RECURSION_LIMIT_EXCEEDED);
        goto error;
    }

    comp->maxEvalDepth = stats.maxEvalDepth;

    ret = opIndex;

error:
    ctxt->depth = oldDepth;

    comp->root = ret;
    return(ret);
}

/**
 * xmlXPathCtxtCompile:
 * @ctxt: an XPath context
 * @str:  the XPath expression
 *
 * Compile an XPath expression
 *
 * Returns the xmlXPathCompExprPtr resulting from the compilation or NULL.
 *         the caller has to free the object.
 */
xmlXPathCompExprPtr
xmlXPathCtxtCompile(xmlXPathContextPtr ctxt, const xmlChar *str) {
    xmlXPathContextPtr tmpctxt = NULL;
    xmlXPathCompExprPtr comp = NULL;
    xmlXPathCompExprPtr oldComp;
    int oldError;

    /*
     * We need an xmlXPathContext for the depth check.
     */
    if (ctxt == NULL) {
        tmpctxt = xmlXPathNewContext(NULL);
        if (tmpctxt == NULL)
            return(NULL);
        ctxt = tmpctxt;
    }

    oldError = ctxt->pctxt.error;
    oldComp = ctxt->pctxt.comp;

    ctxt->pctxt.base = str;
    ctxt->pctxt.cur = str;
    ctxt->pctxt.error = XPATH_EXPRESSION_OK;
    ctxt->pctxt.comp = NULL;

    if (xmlXPathDoCompile(ctxt) < 0)
        goto error;

    ctxt->pctxt.comp->expr = xmlStrdup(str);
    if (ctxt->pctxt.comp->expr == NULL) {
        xmlXPathErrMemory(ctxt);
        goto error;
    }

    comp = ctxt->pctxt.comp;
    ctxt->pctxt.comp = NULL;

error:
    if (ctxt->pctxt.comp != NULL)
        xmlXPathFreeCompExpr(ctxt->pctxt.comp);

    ctxt->pctxt.base = NULL;
    ctxt->pctxt.cur = NULL;
    ctxt->pctxt.error = oldError;
    ctxt->pctxt.comp = oldComp;

    if (tmpctxt != NULL)
        xmlXPathFreeContext(tmpctxt);

    return(comp);
}

/**
 * xmlXPathCompile:
 * @str:  the XPath expression
 *
 * Compile an XPath expression
 *
 * Returns the xmlXPathCompExprPtr resulting from the compilation or NULL.
 *         the caller has to free the object.
 */
xmlXPathCompExprPtr
xmlXPathCompile(const xmlChar *str) {
    return(xmlXPathCtxtCompile(NULL, str));
}

/**
 * xmlXPathCompiledEvalInternal:
 * @comp:  the compiled XPath expression
 * @ctxt:  the XPath context
 * @resObj: the resulting XPath object or NULL
 * @toBool: 1 if only a boolean result is requested
 *
 * Evaluate the Precompiled XPath expression in the given context.
 * The caller has to free @resObj.
 *
 * Returns the xmlXPathObjectPtr resulting from the evaluation or NULL.
 *         the caller has to free the object.
 */
static int
xmlXPathCompiledEvalInternal(xmlXPathCompExprPtr comp,
			     xmlXPathContextPtr ctxt,
			     xmlXPathObjectPtr *resObjPtr,
			     int toBool)
{
    xmlXPathCompExprPtr oldComp;
    int res, oldError;

    if (comp == NULL)
	return(-1);

    xmlResetError(&ctxt->lastError);

    oldError = ctxt->pctxt.error;
    oldComp = ctxt->pctxt.comp;

    ctxt->pctxt.comp = comp;
    ctxt->pctxt.error = XPATH_EXPRESSION_OK;

    res = xmlXPathRunEval(ctxt, resObjPtr, toBool);

    ctxt->pctxt.error = oldError;
    ctxt->pctxt.comp = oldComp;

    return(res);
}

/**
 * xmlXPathCompiledEval:
 * @comp:  the compiled XPath expression
 * @ctx:  the XPath context
 *
 * Evaluate the Precompiled XPath expression in the given context.
 *
 * Returns the xmlXPathObjectPtr resulting from the evaluation or NULL.
 *         the caller has to free the object.
 */
xmlXPathObjectPtr
xmlXPathCompiledEval(xmlXPathCompExprPtr comp, xmlXPathContextPtr ctx)
{
    xmlXPathObjectPtr res = NULL;

    xmlXPathCompiledEvalInternal(comp, ctx, &res, 0);
    return(res);
}

/**
 * xmlXPathCompiledEvalToBoolean:
 * @comp:  the compiled XPath expression
 * @ctxt:  the XPath context
 *
 * Applies the XPath boolean() function on the result of the given
 * compiled expression.
 *
 * Returns 1 if the expression evaluated to true, 0 if to false and
 *         -1 in API and internal errors.
 */
int
xmlXPathCompiledEvalToBoolean(xmlXPathCompExprPtr comp,
			      xmlXPathContextPtr ctxt)
{
    return(xmlXPathCompiledEvalInternal(comp, ctxt, NULL, 1));
}

/**
 * xmlXPathEvalExpr:
 * @ctxt:  the XPath Parser context
 *
 * DEPRECATED: Internal function, don't use.
 */
void
xmlXPathEvalExpr(xmlXPathParserContextPtr ctxt ATTRIBUTE_UNUSED) {
}

/**
 * xmlXPathEval:
 * @str:  the XPath expression
 * @ctx:  the XPath context
 *
 * Evaluate the XPath Location Path in the given context.
 *
 * Returns the xmlXPathObjectPtr resulting from the evaluation or NULL.
 *         the caller has to free the object.
 */
xmlXPathObjectPtr
xmlXPathEval(const xmlChar *str, xmlXPathContextPtr ctxt) {
    xmlXPathObjectPtr res = NULL;
    xmlXPathCompExprPtr oldComp;
    int oldError;

    if (ctxt == NULL)
        return(NULL);

    xmlResetError(&ctxt->lastError);

    oldError = ctxt->pctxt.error;
    oldComp = ctxt->pctxt.comp;

    ctxt->pctxt.base = str;
    ctxt->pctxt.cur = str;
    ctxt->pctxt.error = XPATH_EXPRESSION_OK;
    ctxt->pctxt.comp = NULL;

    if (xmlXPathDoCompile(ctxt) >= 0)
        xmlXPathRunEval(ctxt, &res, 0);

    xmlXPathFreeCompExpr(ctxt->pctxt.comp);

    ctxt->pctxt.base = NULL;
    ctxt->pctxt.cur = NULL;
    ctxt->pctxt.error = oldError;
    ctxt->pctxt.comp = oldComp;

    return(res);
}

/**
 * xmlXPathSetContextNode:
 * @node: the node to to use as the context node
 * @ctx:  the XPath context
 *
 * Sets 'node' as the context node. The node must be in the same
 * document as that associated with the context.
 *
 * Returns -1 in case of error or 0 if successful
 */
int
xmlXPathSetContextNode(xmlNodePtr node, xmlXPathContextPtr ctx) {
    if ((node == NULL) || (ctx == NULL))
        return(-1);

    ctx->node = node;
    return(0);
}

/**
 * xmlXPathNodeEval:
 * @node: the node to to use as the context node
 * @str:  the XPath expression
 * @ctx:  the XPath context
 *
 * Evaluate the XPath Location Path in the given context. The node 'node'
 * is set as the context node. The context node is not restored.
 *
 * Returns the xmlXPathObjectPtr resulting from the evaluation or NULL.
 *         the caller has to free the object.
 */
xmlXPathObjectPtr
xmlXPathNodeEval(xmlNodePtr node, const xmlChar *str, xmlXPathContextPtr ctx) {
    if (str == NULL)
        return(NULL);
    if (xmlXPathSetContextNode(node, ctx) < 0)
        return(NULL);
    return(xmlXPathEval(str, ctx));
}

/**
 * xmlXPathEvalExpression:
 * @str:  the XPath expression
 * @ctxt:  the XPath context
 *
 * Alias for xmlXPathEval().
 *
 * Returns the xmlXPathObjectPtr resulting from the evaluation or NULL.
 *         the caller has to free the object.
 */
xmlXPathObjectPtr
xmlXPathEvalExpression(const xmlChar *str, xmlXPathContextPtr ctxt) {
    return(xmlXPathEval(str, ctxt));
}

/**
 * xmlXPathRegisterAllFunctions:
 * @ctxt:  the XPath context
 *
 * DEPRECATED: No-op since 2.14.0.
 *
 * Registers all default XPath functions in this context
 */
void
xmlXPathRegisterAllFunctions(xmlXPathContextPtr ctxt ATTRIBUTE_UNUSED)
{
}

#endif /* LIBXML_XPATH_ENABLED */
