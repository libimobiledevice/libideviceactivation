/**
 * @file activation.c
 *
 * Copyright (c) 2016-2019 Nikias Bassen, All Rights Reserved.
 * Copyright (c) 2014-2015 Martin Szulecki, All Rights Reserved.
 * Copyright (c) 2011-2014 Mirell Development, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/HTMLtree.h>
#include <curl/curl.h>

#ifdef WIN32
#define IDEVICE_ACTIVATION_API __declspec( dllexport )
#else
#ifdef HAVE_FVISIBILITY
#define IDEVICE_ACTIVATION_API __attribute__((visibility("default")))
#else
#define IDEVICE_ACTIVATION_API
#endif
#endif

#ifdef WIN32
#include <windows.h>
#define strncasecmp _strnicmp
#else
#include <pthread.h>
#endif

#include <libideviceactivation.h>

#define IDEVICE_ACTIVATION_USER_AGENT_IOS "iOS Device Activator (MobileActivation-592.103.2)"
#define IDEVICE_ACTIVATION_USER_AGENT_ITUNES "iTunes/11.1.4 (Macintosh; OS X 10.9.1) AppleWebKit/537.73.11"
#define IDEVICE_ACTIVATION_DEFAULT_URL "https://albert.apple.com/deviceservices/deviceActivation"
#define IDEVICE_ACTIVATION_DRM_HANDSHAKE_DEFAULT_URL "https://albert.apple.com/deviceservices/drmHandshake"

typedef enum {
	IDEVICE_ACTIVATION_CONTENT_TYPE_URL_ENCODED,
	IDEVICE_ACTIVATION_CONTENT_TYPE_MULTIPART_FORMDATA,
	IDEVICE_ACTIVATION_CONTENT_TYPE_HTML,
	IDEVICE_ACTIVATION_CONTENT_TYPE_BUDDYML,
	IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST,
	IDEVICE_ACTIVATION_CONTENT_TYPE_UNKNOWN
} idevice_activation_content_type_t;

struct idevice_activation_request_private {
	idevice_activation_client_type_t client_type;
	idevice_activation_content_type_t content_type;
	char* url;
	plist_t fields;
};

struct idevice_activation_response_private {
	char* raw_content;
	size_t raw_content_size;
	idevice_activation_content_type_t content_type;
	char* title;
	char* description;
	plist_t activation_record;
	plist_t headers;
	plist_t fields;
	plist_t fields_require_input;
	plist_t fields_secure_input;
	plist_t labels;
	plist_t labels_placeholder;
	int is_activation_ack;
	int is_auth_required;
	int has_errors;
};


static void internal_libideviceactivation_init(void)
{
	curl_global_init(CURL_GLOBAL_ALL);
}

static void internal_libideviceactivation_deinit(void)
{
	curl_global_cleanup();
}

#ifdef WIN32
typedef volatile struct {
    LONG lock;
    int state;
} thread_once_t;

static thread_once_t init_once = {0, 0};
static thread_once_t deinit_once = {0, 0};

static void thread_once(thread_once_t *once_control, void (*init_routine)(void))
{
    while (InterlockedExchange(&(once_control->lock), 1) != 0) {
        Sleep(1);
    }
    if (!once_control->state) {
        once_control->state = 1;
        init_routine();
    }
    InterlockedExchange(&(once_control->lock), 0);
}
#else
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static pthread_once_t deinit_once = PTHREAD_ONCE_INIT;
#define thread_once pthread_once
#endif

#ifndef HAVE_ATTRIBUTE_CONSTRUCTOR
  #if defined(__llvm__) || defined(__GNUC__)
    #define HAVE_ATTRIBUTE_CONSTRUCTOR
  #endif
#endif

#ifdef HAVE_ATTRIBUTE_CONSTRUCTOR
static void __attribute__((constructor)) libideviceactivation_initialize(void)
{
    thread_once(&init_once, internal_libideviceactivation_init);
}

static void __attribute__((destructor)) libideviceactivation_deinitialize(void)
{
    thread_once(&deinit_once, internal_libideviceactivation_deinit);
}
#elif defined(WIN32)
BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        thread_once(&init_once, internal_libideviceactivation_init);
        break;
    case DLL_PROCESS_DETACH:
        thread_once(&deinit_once, internal_libideviceactivation_deinit);
        break;
    default:
        break;
    }
    return 1;
}
#else
#warning No compiler support for constructor/destructor attributes, some features might not be available.
#endif

static int debug_level = 0;

IDEVICE_ACTIVATION_API void idevice_activation_set_debug_level(int level) {
	debug_level = level;
}

static idevice_activation_error_t idevice_activation_activation_record_from_plist(idevice_activation_response_t response, plist_t plist)
{
	plist_t record = plist_dict_get_item(plist, "ActivationRecord");
	if (record != NULL) {
		plist_t ack_received = plist_dict_get_item(record, "ack-received");
		if (ack_received) {
			uint8_t val = 0;
			plist_get_bool_val(ack_received, &val);
			if (val) {
				response->is_activation_ack = 1;
			}
		}
		response->activation_record = plist_new_data(response->raw_content, response->raw_content_size);
	} else {
		plist_t activation_node = plist_dict_get_item(plist, "iphone-activation");
		if (!activation_node) {
			activation_node = plist_dict_get_item(plist, "device-activation");
		}
		if (!activation_node) {
			return IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
		}
		plist_t ack_received = plist_dict_get_item(activation_node, "ack-received");
		if (ack_received) {
			uint8_t val = 0;
			plist_get_bool_val(ack_received, &val);
			if (val) {
				response->is_activation_ack = 1;
			}
		}
		record = plist_dict_get_item(activation_node, "activation-record");
		if (record) {
			response->activation_record = plist_copy(record);
		}
	}
	return IDEVICE_ACTIVATION_E_SUCCESS;
}

static void idevice_activation_response_add_field(idevice_activation_response_t response, const char* key, const char* value, int required_input, int secure_input)
{
	plist_dict_set_item(response->fields, key, plist_new_string(value));
	if (required_input) {
		plist_dict_set_item(response->fields_require_input, key, plist_new_bool(1));
	}
	if (secure_input) {
		plist_dict_set_item(response->fields_secure_input, key, plist_new_bool(1));
	}
}

static idevice_activation_error_t idevice_activation_parse_buddyml_response(idevice_activation_response_t response)
{
	idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;
	xmlDocPtr doc = NULL;
	xmlXPathContextPtr context = NULL;
	xmlXPathObjectPtr xpath_result = NULL;
	int i = 0;

	if (response->content_type != IDEVICE_ACTIVATION_CONTENT_TYPE_BUDDYML)
		return IDEVICE_ACTIVATION_E_UNKNOWN_CONTENT_TYPE;

	doc = xmlReadMemory(response->raw_content, response->raw_content_size, "ideviceactivation.xml", NULL, XML_PARSE_NOERROR);
	if (!doc) {
		result = IDEVICE_ACTIVATION_E_BUDDYML_PARSING_ERROR;
		goto cleanup;
	}

	context = xmlXPathNewContext(doc);
	if (!context) {
		result = IDEVICE_ACTIVATION_E_BUDDYML_PARSING_ERROR;
		goto cleanup;
	}

	// check for an error
	// <navigationBar> appears directly under <xmlui> only in case of an error
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/navigationBar/@title", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval && xpath_result->nodesetval->nodeNr) {
		xmlChar* content =  xmlNodeListGetString(doc, xpath_result->nodesetval->nodeTab[0]->xmlChildrenNode, 1);
		if (content) {
			response->title = strdup((const char*) content);
			xmlFree(content);
		}

		response->has_errors = 1;
		goto cleanup;
	}

	// check for activation ack
	if (xpath_result) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = NULL;
	}
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/clientInfo[@ack-received='true']", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval && xpath_result->nodesetval->nodeNr) {
		// existing activation_acknowledged
		response->is_activation_ack = 1;
		goto cleanup;
	}

	// retrieve response title
	if (xpath_result) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = NULL;
	}
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/alert/@title", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval && xpath_result->nodesetval->nodeNr) {
		// incorrect credentials
		// <alert> exists only in case of incorrect credentials
		xmlChar* content =  xmlNodeListGetString(doc, xpath_result->nodesetval->nodeTab[0]->xmlChildrenNode, 1);
		if (content) {
			response->title = strdup((const char*) content);
			xmlFree(content);
		}
	} else {
		if (xpath_result) {
			xmlXPathFreeObject(xpath_result);
			xpath_result = NULL;
		}
		xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/page/navigationBar/@title", context);
		if (!xpath_result) {
			result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
			goto cleanup;
		}

		if (!xpath_result->nodesetval) {
			result =  IDEVICE_ACTIVATION_E_BUDDYML_PARSING_ERROR;
			goto cleanup;
		}
		xmlChar* content =  xmlNodeListGetString(doc, xpath_result->nodesetval->nodeTab[0]->xmlChildrenNode, 1);
		if (content) {
			response->title = strdup((const char*) content);
			xmlFree(content);
		}
	}

	// retrieve response description
	if (xpath_result) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = NULL;
	}
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/page/tableView/section/footer[not (@url)]", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}
	if (!xpath_result->nodesetval) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/page/tableView/section[@footer and not(@footerLinkURL)]/@footer", context);
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval) {
		char* response_description = (char*) malloc(sizeof(char));
		response_description[0] = '\0';

		for(i = 0; i < xpath_result->nodesetval->nodeNr; i++) {
			xmlChar* content = xmlNodeListGetString(doc, xpath_result->nodesetval->nodeTab[i]->xmlChildrenNode, 1);
			if (content) {
				const size_t len = strlen(response_description);
				response_description = (char*) realloc(response_description, len + xmlStrlen(content) + 2);
				sprintf(&response_description[len], "%s\n", (const char*) content);
				xmlFree(content);
			}
		}

		if (strlen(response_description) > 0) {
			// remove the last '\n'
			response_description[strlen(response_description) - 1] = '\0';
			response->description = response_description;
		} else {
			free(response_description);
		}
	}

	// retrieve input fields
	if (xpath_result) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = NULL;
	}

	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/page//editableTextRow", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval) {
		for(i = 0; i < xpath_result->nodesetval->nodeNr; i++) {
			xmlChar* id = xmlGetProp(xpath_result->nodesetval->nodeTab[i], (const xmlChar*) "id");
			if (!id) {
				result = IDEVICE_ACTIVATION_E_BUDDYML_PARSING_ERROR;
				goto cleanup;
			}
			int secure_input = 0;
			xmlChar* secure = xmlGetProp(xpath_result->nodesetval->nodeTab[i], (const xmlChar*) "secure");
			if (secure) {
				if (!strcmp((const char*)secure, "true")) {
					secure_input = 1;
				}
				xmlFree(secure);
			}

			idevice_activation_response_add_field(response, (const char*) id, "", 1, secure_input);

			xmlChar* label = xmlGetProp(xpath_result->nodesetval->nodeTab[i], (const xmlChar*) "label");
			if (label) {
				plist_dict_set_item(response->labels, (const char*)id, plist_new_string((const char*) label));
				xmlFree(label);
			}
			xmlChar* placeholder = xmlGetProp(xpath_result->nodesetval->nodeTab[i], (const xmlChar*) "placeholder");
			if (placeholder) {
				plist_dict_set_item(response->labels_placeholder, (const char*)id, plist_new_string((const char*) placeholder));
				xmlFree(placeholder);
			}

			xmlFree(id);
		}
	}

	// retrieve server info fields
	if (xpath_result) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = NULL;
	}

	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/serverInfo/@*", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval) {
		for(i = 0; i < xpath_result->nodesetval->nodeNr; i++) {
			xmlChar* content = xmlNodeGetContent(xpath_result->nodesetval->nodeTab[i]);
			if (content) {
				if (!xmlStrcmp(xpath_result->nodesetval->nodeTab[i]->name, (const xmlChar*) "isAuthRequired")) {
					response->is_auth_required = 1;
				}

				idevice_activation_response_add_field(response,
					(const char*) xpath_result->nodesetval->nodeTab[i]->name, (const char*) content, 0, 0);
				xmlFree(content);
			}
		}
	}

	if (plist_dict_get_size(response->fields) == 0) {
		response->has_errors = 1;
	}

cleanup:
	if (xpath_result)
		xmlXPathFreeObject(xpath_result);
	if (context)
		xmlXPathFreeContext(context);
	if (doc)
		xmlFreeDoc(doc);

	return result;
}

static idevice_activation_error_t idevice_activation_parse_html_response(idevice_activation_response_t response)
{
	idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;
	xmlDocPtr doc = NULL;
	xmlXPathContextPtr context = NULL;
	xmlXPathObjectPtr xpath_result = NULL;

	if (response->content_type != IDEVICE_ACTIVATION_CONTENT_TYPE_HTML)
		return IDEVICE_ACTIVATION_E_UNKNOWN_CONTENT_TYPE;

	doc = xmlReadMemory(response->raw_content, response->raw_content_size, "ideviceactivation.xml", NULL, XML_PARSE_RECOVER | XML_PARSE_NOERROR);
	if (!doc) {
		result = IDEVICE_ACTIVATION_E_HTML_PARSING_ERROR;
		goto cleanup;
	}

	context = xmlXPathNewContext(doc);
	if (!context) {
		result = IDEVICE_ACTIVATION_E_HTML_PARSING_ERROR;
		goto cleanup;
	}

	// check for authentication required
	if (xpath_result) {
		xmlXPathFreeObject(xpath_result);
		xpath_result = NULL;
	}
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "//input[@name='isAuthRequired' and @value='true']", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval && xpath_result->nodesetval->nodeNr) {
		response->is_auth_required = 1;
		goto cleanup;
	}

	// check for plist content
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "//script[@type='text/x-apple-plist']/plist", context);
	if (!xpath_result) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	if (xpath_result->nodesetval && xpath_result->nodesetval->nodeNr) {
		plist_t plist = NULL;
		xmlBufferPtr plistNodeBuffer = xmlBufferCreate();
		if (htmlNodeDump(plistNodeBuffer, doc, xpath_result->nodesetval->nodeTab[0]) == -1) {
			result = IDEVICE_ACTIVATION_E_HTML_PARSING_ERROR;
			goto local_cleanup;
		}

		plist_from_xml((const char*) plistNodeBuffer->content, plistNodeBuffer->use, &plist);
		if (!plist) {
			result = IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
			goto local_cleanup;
		}
		result = idevice_activation_activation_record_from_plist(response, plist);
		plist_free(plist);

	local_cleanup:
		if (plistNodeBuffer)
			xmlBufferFree(plistNodeBuffer);
		goto cleanup;
	}

	response->has_errors = 1;

cleanup:
	if (xpath_result)
		xmlXPathFreeObject(xpath_result);
	if (context)
		xmlXPathFreeContext(context);
	if (doc)
		xmlFreeDoc(doc);

	return result;
}

static idevice_activation_error_t idevice_activation_parse_raw_response(idevice_activation_response_t response)
{
	switch(response->content_type)
	{
		case IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST:
		{
			idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;

			plist_t plist = NULL;
			plist_from_xml(response->raw_content, response->raw_content_size, &plist);

			if (plist == NULL) {
				return IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
			}

			/* check if this is a reply to drmHandshake request */
			if (plist_dict_get_item(plist, "HandshakeResponseMessage") != NULL) {
				result = IDEVICE_ACTIVATION_E_SUCCESS;
			} else {
				result = idevice_activation_activation_record_from_plist(response, plist);
			}

			plist_free(response->fields);
			response->fields = plist;

			return result;
		}
		case IDEVICE_ACTIVATION_CONTENT_TYPE_BUDDYML:
			return idevice_activation_parse_buddyml_response(response);
		case IDEVICE_ACTIVATION_CONTENT_TYPE_HTML:
			return idevice_activation_parse_html_response(response);
		default:
			return IDEVICE_ACTIVATION_E_UNKNOWN_CONTENT_TYPE;
	}

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

static size_t idevice_activation_write_callback(char* data, size_t size, size_t nmemb, void* userdata)
{
	idevice_activation_response_t response = (idevice_activation_response_t)userdata;
	const size_t total = size * nmemb;

	if (total != 0) {
		response->raw_content = realloc(response->raw_content, response->raw_content_size + total + 1);
		memcpy(response->raw_content + response->raw_content_size, data, total);
		response->raw_content[response->raw_content_size + total] = '\0';
		response->raw_content_size += total;
	}

	return total;
}

static size_t idevice_activation_header_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
	idevice_activation_response_t response = (idevice_activation_response_t)userdata;
	const size_t total = size * nmemb;
	if (total != 0) {
		char *header = malloc(total + 1);
		char *value = NULL;
		char *p = NULL;
		memcpy(header, data, total);
		header[total] = '\0';

		p = strchr(header, ':');
		if (p) {
			*p = '\0';
			p++;
			while (*p == ' ') {
				p++;
			}
			if (*p != '\0') {
				value = p;
				while (*p != '\0' && *p != '\n' && *p != '\r') {
					p++;
				}
				*p = '\0';
			}
		}
		if (value) {
			if (strncasecmp(header, "Content-Type", 12) == 0) {
				if (strncasecmp(value, "text/xml", 8) == 0) {
					response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST;
				} else if (strncasecmp(value, "application/xml", 15) == 0) {
					response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST;
				} else if (strncasecmp(value, "application/x-buddyml", 21) == 0) {
					response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_BUDDYML;
				} else if (strncasecmp(value, "text/html", 9) == 0) {
					response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_HTML;
				}
			}
			plist_dict_set_item(response->headers, header, plist_new_string(value));
		}
		free(header);
	}
	return total;
}

static char* urlencode(const char* buf)
{
	static const signed char conv_table[256] = {
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
		1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
		1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
	};

	unsigned int i;
	int count = 0;
	for (i = 0; i < strlen(buf); i++) {
		if (conv_table[(int)buf[i]]) {
			count++;
		}
	}
	int newsize = strlen(buf) + count*2 + 1;
	char* res = malloc(newsize);
	int o = 0;
	for (i = 0; i < strlen(buf); i++) {
		if (conv_table[(int)buf[i]]) {
			sprintf(&res[o], "%%%02X", (unsigned char)buf[i]);
			o+=3;
		} else {
			res[o] = buf[i];
			o++;
		}
	}
	res[o] = '\0';

	return res;
}

static int plist_strip_xml(char** xmlplist)
{
	if (!xmlplist || !*xmlplist)
		return -1;

	char* start = strstr(*xmlplist, "<plist version=\"1.0\">\n");
	if (!start)
		return -1;

	char* stop = strstr(*xmlplist, "\n</plist>");
	if (!stop)
		return -1;

	start += strlen("<plist version=\"1.0\">\n");
	uint32_t size = stop - start;
	char* stripped = malloc(size + 1);
	if (!stripped)
		return -1;

	memcpy(stripped, start, size);
	stripped[size] = '\0';
	free(*xmlplist);
	*xmlplist = stripped;

	return 0;
}

static int idevice_activation_curl_debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr)
{
	switch (type) {
	case CURLINFO_TEXT:
		fprintf(stderr, "* ");
		break;
	case CURLINFO_DATA_IN:
	case CURLINFO_HEADER_IN:
		fprintf(stderr, "< ");
		break;
	case CURLINFO_DATA_OUT:
	case CURLINFO_HEADER_OUT:
		fprintf(stderr, "> ");
		break;
	case CURLINFO_SSL_DATA_IN:
	case CURLINFO_SSL_DATA_OUT:
		return 0;
	default:
		return 0;
	}

	fwrite(data, 1, size, stderr);
	if (size > 0 && data[size-1] != '\n') {
		fprintf(stderr, "\n");
	}
	return 0;
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_request_new(idevice_activation_client_type_t client_type, idevice_activation_request_t* request)
{
	if (!request)
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;

	idevice_activation_request_t tmp_request = (idevice_activation_request_t) malloc(sizeof(idevice_activation_request));

	if (!tmp_request) {
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	tmp_request->client_type = client_type;
	tmp_request->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_URL_ENCODED;
	tmp_request->url = strdup(IDEVICE_ACTIVATION_DEFAULT_URL);
	tmp_request->fields = plist_new_dict();
	*request = tmp_request;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_request_new_from_lockdownd(idevice_activation_client_type_t client_type, lockdownd_client_t lockdown, idevice_activation_request** request)
{
	if (!lockdown || !request) {
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
	}

	uint8_t has_telephony_capability = 0;
	uint8_t has_mobile_equipment_id = 0;
	lockdownd_error_t err;
	plist_t info = NULL;
	plist_t node = NULL;
	plist_t fields = plist_new_dict();

	// add InStoreActivation
	plist_dict_set_item(fields, "InStoreActivation", plist_new_string("false"));

	// get a bunch of information at once
	err = lockdownd_get_value(lockdown, NULL, NULL, &info);
	if (err != LOCKDOWN_E_SUCCESS) {
		if (debug_level > 0)
			fprintf(stderr, "%s: Unable to get basic information from lockdownd\n", __func__);
		plist_free(fields);
		return IDEVICE_ACTIVATION_E_INCOMPLETE_INFO;
	}

	// add AppleSerialNumber
	node = plist_dict_get_item(info, "SerialNumber");
	if (!node || plist_get_node_type(node) != PLIST_STRING) {
		if (debug_level > 0)
			fprintf(stderr, "%s: Unable to get SerialNumber from lockdownd\n", __func__);
		plist_free(fields);
		plist_free(info);
		return IDEVICE_ACTIVATION_E_INCOMPLETE_INFO;
	}
	plist_dict_set_item(fields, "AppleSerialNumber", plist_copy(node));
	node = NULL;


	// check if device has telephone capability
	node = plist_dict_get_item(info, "TelephonyCapability");
	if (!node || plist_get_node_type(node) != PLIST_BOOLEAN) {
		has_telephony_capability = 0;
	} else {
		plist_get_bool_val(node, &has_telephony_capability);
	}
	node = NULL;

	if (has_telephony_capability) {
		// add IMEI
		node = plist_dict_get_item(info, "InternationalMobileEquipmentIdentity");
		if (!node || plist_get_node_type(node) != PLIST_STRING) {
			has_mobile_equipment_id = 0;
		} else {
			plist_dict_set_item(fields, "IMEI", plist_copy(node));
			has_mobile_equipment_id = 1;
		}
		node = NULL;

		// add MEID
		node = plist_dict_get_item(info, "MobileEquipmentIdentifier");
		if (!node || plist_get_node_type(node) != PLIST_STRING) {
			if (debug_level > 0)
				fprintf(stderr, "%s: Unable to get MEID from lockdownd\n", __func__);
			if (!has_mobile_equipment_id) {
				plist_free(fields);
				plist_free(info);
				return IDEVICE_ACTIVATION_E_INCOMPLETE_INFO;
			}
		} else {
			plist_dict_set_item(fields, "MEID", plist_copy(node));
		}
		node = NULL;

		// add IMSI
		node = plist_dict_get_item(info, "InternationalMobileSubscriberIdentity");
		if (!node || plist_get_node_type(node) != PLIST_STRING) {
			if (debug_level > 0)
				fprintf(stderr, "%s: Unable to get IMSI from lockdownd\n", __func__);
		} else {
			plist_dict_set_item(fields, "IMSI", plist_copy(node));
		}
		node = NULL;

		// add ICCID
		node = plist_dict_get_item(info, "IntegratedCircuitCardIdentity");
		if (!node || plist_get_node_type(node) != PLIST_STRING) {
			if (debug_level > 0)
				fprintf(stderr, "%s: Unable to get ICCID from lockdownd\n", __func__);
		} else {
			plist_dict_set_item(fields, "ICCID", plist_copy(node));
		}
		node = NULL;
	}
	plist_free(info);
	info = NULL;

	// add activation-info
	err = lockdownd_get_value(lockdown, NULL, "ActivationInfo", &node);
	if (err != LOCKDOWN_E_SUCCESS || !node || plist_get_node_type(node) != PLIST_DICT) {
		fprintf(stderr, "%s: Unable to get ActivationInfo from lockdownd\n", __func__);
		plist_free(fields);
		return IDEVICE_ACTIVATION_E_INCOMPLETE_INFO;
	}
	plist_dict_set_item(fields, "activation-info", plist_copy(node));
	plist_free(node);
	node = NULL;

	idevice_activation_request* tmp_request = (idevice_activation_request*) malloc(sizeof(idevice_activation_request));

	if (!tmp_request) {
		plist_free(fields);
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	tmp_request->client_type = client_type;
	tmp_request->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_MULTIPART_FORMDATA;
	tmp_request->url = strdup(IDEVICE_ACTIVATION_DEFAULT_URL);
	tmp_request->fields = fields;
	*request = tmp_request;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_drm_handshake_request_new(idevice_activation_client_type_t client_type, idevice_activation_request_t* request)
{
	if (!request)
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;

	idevice_activation_request_t tmp_request = (idevice_activation_request_t) malloc(sizeof(idevice_activation_request));

	if (!tmp_request) {
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	tmp_request->client_type = client_type;
	tmp_request->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST;
	tmp_request->url = strdup(IDEVICE_ACTIVATION_DRM_HANDSHAKE_DEFAULT_URL);
	tmp_request->fields = plist_new_dict();
	*request = tmp_request;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

IDEVICE_ACTIVATION_API void idevice_activation_request_free(idevice_activation_request_t request)
{
	if (!request)
		return;

	plist_free(request->fields);
	free(request);
}

IDEVICE_ACTIVATION_API void idevice_activation_request_get_fields(idevice_activation_request_t request, plist_t* fields)
{
	if (!request || !fields)
		return;

	*fields = plist_copy(request->fields);
}

IDEVICE_ACTIVATION_API void idevice_activation_request_set_fields(idevice_activation_request_t request, plist_t fields)
{
	if (!request || !fields)
		return;

	if (request->content_type == IDEVICE_ACTIVATION_CONTENT_TYPE_URL_ENCODED) {
		// if at least one of the new fields has a different type than string, we have to change the type
		plist_dict_iter iter = NULL;
		plist_dict_new_iter(fields, &iter);
		plist_t item = NULL;
		do {
			plist_dict_next_item(fields, iter, NULL, &item);
			if (item && plist_get_node_type(item) != PLIST_STRING) {
				request->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_MULTIPART_FORMDATA;
				break;
			}
		} while(item);
	}

	plist_dict_merge(&request->fields, fields);
}

IDEVICE_ACTIVATION_API void idevice_activation_request_set_fields_from_response(idevice_activation_request_t request, const idevice_activation_response_t response)
{
	if (!request || !response)
		return;

	plist_t response_fields = NULL;
	idevice_activation_response_get_fields(response, &response_fields);
	if (response_fields) {
		idevice_activation_request_set_fields(request, response_fields);
		free(response_fields);
	}
}

IDEVICE_ACTIVATION_API void idevice_activation_request_set_field(idevice_activation_request_t request, const char* key, const char* value)
{
	if (!request || !key || !value)
		return;

	plist_dict_set_item(request->fields, key, plist_new_string(value));
}

IDEVICE_ACTIVATION_API void idevice_activation_request_get_field(idevice_activation_request_t request, const char* key, char** value)
{
	if (!request || !key || !value)
		return;

	char* tmp_value = NULL;

	plist_t item = plist_dict_get_item(request->fields, key);

	if (item && plist_get_node_type(item) == PLIST_STRING) {
		plist_get_string_val(item, &tmp_value);
	} else {
		uint32_t data_size = 0;
		plist_to_xml(item, &tmp_value, &data_size);
		plist_strip_xml(&tmp_value);
	}

	*value = tmp_value;
}

IDEVICE_ACTIVATION_API void idevice_activation_request_get_url(idevice_activation_request_t request, const char** url)
{
	if (!request || !url)
		return;

	*url = request->url;
}

IDEVICE_ACTIVATION_API void idevice_activation_request_set_url(idevice_activation_request_t request, const char* url)
{
	if (!request || !url)
		return;

	free(request->url);
	request->url = strdup(url);
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_response_new(idevice_activation_response_t* response)
{
	if (!response)
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;

	idevice_activation_response_t tmp_response = (idevice_activation_response_t) malloc(sizeof(idevice_activation_response));

	if (!tmp_response) {
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	tmp_response->raw_content = NULL;
	tmp_response->raw_content_size = 0;
	tmp_response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_UNKNOWN;
	tmp_response->title = NULL;
	tmp_response->description = NULL;
	tmp_response->activation_record = NULL;
	tmp_response->headers = plist_new_dict();
	tmp_response->fields = plist_new_dict();
	tmp_response->fields_require_input = plist_new_dict();
	tmp_response->fields_secure_input = plist_new_dict();
	tmp_response->labels = plist_new_dict();
	tmp_response->labels_placeholder = plist_new_dict();
	tmp_response->is_activation_ack = 0;
	tmp_response->is_auth_required = 0;
	tmp_response->has_errors = 0;
	*response = tmp_response;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_response_new_from_html(const char* content, idevice_activation_response_t* response)
{
	if (!content || !response)
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;

	idevice_activation_response_t tmp_response = NULL;
	idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;

	result = idevice_activation_response_new(&tmp_response);
	if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
		return result;
	}

	const size_t tmp_size = strlen(content);
	char* tmp_content = (char*) malloc(sizeof(char) * tmp_size);

	if (!tmp_content) {
		idevice_activation_response_free(tmp_response);
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	memcpy(tmp_content, content, tmp_size);

	tmp_response->raw_content = tmp_content;
	tmp_response->raw_content_size = tmp_size;
	tmp_response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_HTML;

	result = idevice_activation_parse_html_response(tmp_response);
	if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
		idevice_activation_response_free(tmp_response);
		return result;
	}

	*response = tmp_response;

	return result;
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_response_to_buffer(idevice_activation_response_t response, char** buffer, size_t* size)
{
	if (!response || !buffer || !size)
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;

	char* tmp_buffer = (char*) malloc(sizeof(char) * response->raw_content_size);
	if (!tmp_buffer) {
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	memcpy(tmp_buffer, response->raw_content, response->raw_content_size);

	*buffer = tmp_buffer;
	*size = response->raw_content_size;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

IDEVICE_ACTIVATION_API void idevice_activation_response_free(idevice_activation_response_t response)
{
	if (!response)
		return;

	free(response->raw_content);
	free(response->title);
	free(response->description);
	plist_free(response->activation_record);
	plist_free(response->headers);
	plist_free(response->fields);
	plist_free(response->fields_require_input);
	plist_free(response->fields_secure_input);
	plist_free(response->labels);
	plist_free(response->labels_placeholder);
	free(response);
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_field(idevice_activation_response_t response, const char* key, char** value)
{
	if (!response || !key || !value)
		return;

	*value = NULL;
	plist_t item = plist_dict_get_item(response->fields, key);

	if (item && plist_get_node_type(item) == PLIST_STRING) {
		plist_get_string_val(item, value);
	}
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_fields(idevice_activation_response_t response, plist_t* fields)
{
	if (response && response->fields && fields) {
		*fields = plist_copy(response->fields);
	}
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_label(idevice_activation_response_t response, const char* key, char** value)
{
	if (!response || !key || !value)
		return;

	*value = NULL;
	plist_t item = plist_dict_get_item(response->labels, key);
	if (item) {
		plist_get_string_val(item, value);
	}
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_placeholder(idevice_activation_response_t response, const char* key, char** value)
{
	if (!response || !key || !value)
		return;

	*value = NULL;
	plist_t item = plist_dict_get_item(response->labels_placeholder, key);
	if (item) {
		plist_get_string_val(item, value);
	}
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_title(idevice_activation_response_t response, const char** title)
{
	if (!response || !title)
		return;

	*title = response->title;
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_description(idevice_activation_response_t response, const char** description)
{
	if (!response || !description)
		return;

	*description = response->description;
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_activation_record(idevice_activation_response_t response, plist_t* activation_record)
{
	if (!response || !activation_record)
		return;

	if (response->activation_record) {
		*activation_record = plist_copy(response->activation_record);
	} else {
		*activation_record = NULL;
	}
}

IDEVICE_ACTIVATION_API void idevice_activation_response_get_headers(idevice_activation_response_t response, plist_t* headers)
{
	if (!response || !headers)
		return;

	*headers = plist_copy(response->headers);
}

IDEVICE_ACTIVATION_API int idevice_activation_response_is_activation_acknowledged(idevice_activation_response_t response)
{
	if (!response)
		return 0;

	return response->is_activation_ack;
}

IDEVICE_ACTIVATION_API int idevice_activation_response_is_authentication_required(idevice_activation_response_t response)
{
	if (!response)
		return 0;

	return response->is_auth_required;
}

IDEVICE_ACTIVATION_API int idevice_activation_response_field_requires_input(idevice_activation_response_t response, const char* key)
{
	if (!response || !key)
		return 0;

	return (plist_dict_get_item(response->fields_require_input, key) ? 1 : 0);
}

IDEVICE_ACTIVATION_API int idevice_activation_response_field_secure_input(idevice_activation_response_t response, const char* key)
{
	if (!response || !key)
		return 0;

	return (plist_dict_get_item(response->fields_secure_input, key) ? 1 : 0);
}

IDEVICE_ACTIVATION_API int idevice_activation_response_has_errors(idevice_activation_response_t response)
{
	if (!response)
		return 0;

	return response->has_errors;
}

IDEVICE_ACTIVATION_API idevice_activation_error_t idevice_activation_send_request(idevice_activation_request_t request, idevice_activation_response_t* response)
{
	idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;

	// check arguments
	if (!request || !response) {
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
	}

	plist_dict_iter iter = NULL;
	plist_dict_new_iter(request->fields, &iter);
	if (!iter) {
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
	}

	CURL* handle = curl_easy_init();
	struct curl_httppost* form = NULL;
	struct curl_slist* slist = NULL;

	if (!handle) {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	switch (request->client_type) {
		case IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION:
			curl_easy_setopt(handle, CURLOPT_USERAGENT, IDEVICE_ACTIVATION_USER_AGENT_IOS);
			break;
		case IDEVICE_ACTIVATION_CLIENT_ITUNES:
			curl_easy_setopt(handle, CURLOPT_USERAGENT, IDEVICE_ACTIVATION_USER_AGENT_ITUNES);
			break;
		default:
			result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
			goto cleanup;
	}

	char* key = NULL;
	char* svalue = NULL;
	plist_t value_node = NULL;

	if (request->content_type == IDEVICE_ACTIVATION_CONTENT_TYPE_MULTIPART_FORMDATA) {
		struct curl_httppost* last = NULL;
		do {
			plist_dict_next_item(request->fields, iter, &key, &value_node);
			if (key != NULL) {
				if (value_node != NULL) {
					// serialize plist node as field value
					if (plist_get_node_type(value_node) == PLIST_STRING) {
						plist_get_string_val(value_node, &svalue);
					} else {
						uint32_t data_size = 0;
						plist_to_xml(value_node, &svalue, &data_size);
						plist_strip_xml(&svalue);
					}

					curl_formadd(&form, &last, CURLFORM_COPYNAME, key, CURLFORM_COPYCONTENTS, svalue, CURLFORM_END);

					free(svalue);
					svalue = NULL;
				}
			}
		} while(value_node != NULL);
		curl_easy_setopt(handle, CURLOPT_HTTPPOST, form);

	} else if (request->content_type == IDEVICE_ACTIVATION_CONTENT_TYPE_URL_ENCODED) {
		char* postdata = (char*) malloc(sizeof(char));
		postdata[0] = '\0';
		do {
			plist_dict_next_item(request->fields, iter, &key, &value_node);
			if (key != NULL) {
				if (value_node != NULL) {
					// serialize plist node as field value
					if (plist_get_node_type(value_node) == PLIST_STRING) {
						plist_get_string_val(value_node, &svalue);
					} else {
						// only strings supported
						free(postdata);
						result = IDEVICE_ACTIVATION_E_UNSUPPORTED_FIELD_TYPE;
						goto cleanup;
					}

					char* value_encoded = urlencode(svalue);
					if (value_encoded) {
						const size_t new_size = strlen(postdata) + strlen(key) + strlen(value_encoded) + 3;
						postdata = (char*) realloc(postdata, new_size);
						sprintf(&postdata[strlen(postdata)], "%s=%s&", key, value_encoded);
						free(value_encoded);
					}

					free(svalue);
					svalue = NULL;
				}
			}
		} while(value_node != NULL);

		// remove the last '&'
		const size_t postdata_len = strlen(postdata);
		if (postdata_len > 0)
			postdata[postdata_len - 1] = '\0';

		curl_easy_setopt(handle, CURLOPT_POST, 1);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, postdata);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, strlen(postdata));
	} else if (request->content_type == IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST) {
		char *postdata = NULL;
		uint32_t postdata_len = 0;
		plist_to_xml(request->fields, &postdata, &postdata_len);
		curl_easy_setopt(handle, CURLOPT_POST, 1);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, postdata);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, postdata_len);
		slist = curl_slist_append(NULL, "Content-Type: application/x-apple-plist");
		slist = curl_slist_append(slist, "Accept: application/xml");
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, slist);
	}
	else {
		result = IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	idevice_activation_response_t tmp_response = NULL;
	result = idevice_activation_response_new(&tmp_response);
	if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
		goto cleanup;
	}

	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, tmp_response);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &idevice_activation_write_callback);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, tmp_response);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &idevice_activation_header_callback);
	curl_easy_setopt(handle, CURLOPT_URL, request->url);
	curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 1);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

	// enable communication debugging
	if (debug_level > 0) {
		curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION, idevice_activation_curl_debug_callback);
	}

	curl_easy_perform(handle);

	result = idevice_activation_parse_raw_response(tmp_response);
	if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
		goto cleanup;
	}

	*response = tmp_response;

	result = IDEVICE_ACTIVATION_E_SUCCESS;

cleanup:
	free(iter);
	if (form)
		curl_formfree(form);
	if (slist)
		curl_slist_free_all(slist);
	if (handle)
		curl_easy_cleanup(handle);

	return result;
}
