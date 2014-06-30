/**
 * @file activation.c
 *
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

#include <libideviceactivation.h>

#define IDEVICE_ACTIVATION_USER_AGENT_IOS "iOS Device Activator (MobileActivation-20 built on Jan 15 2012 at 19:07:28)"
#define IDEVICE_ACTIVATION_USER_AGENT_ITUNES "iTunes/11.1.4 (Macintosh; OS X 10.9.1) AppleWebKit/537.73.11"
#define IDEVICE_ACTIVATION_DEFAULT_URL "https://albert.apple.com/deviceservices/deviceActivation"

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
	plist_t fields;
	plist_t fields_require_input;
	plist_t labels;
	int is_activation_ack;
	int is_auth_required;
	int has_errors;
};

int debug_level = 0;

void idevice_activation_set_debug_level(int level) {
	debug_level = level;
}

static idevice_activation_error_t idevice_activation_activation_node_from_plist_xml(const char* plist_xml, size_t size, plist_t* activation_node)
{
	plist_t activation_ticket = NULL;

	plist_from_xml(plist_xml, size, &activation_ticket);
	if (activation_ticket == NULL) {
		return IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
	}

	plist_t activation_node_tmp = plist_dict_get_item(activation_ticket, "iphone-activation");
	if (!activation_node_tmp) {
		activation_node_tmp = plist_dict_get_item(activation_ticket, "device-activation");
		if (!activation_node_tmp) {
			plist_free(activation_ticket);
			return IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
		}
	}

	*activation_node = plist_copy(activation_node_tmp);
	plist_free(activation_ticket);

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

static idevice_activation_error_t idevice_activation_activation_record_from_activation_node(plist_t activation_node, plist_t* activation_record)
{
	plist_t record = plist_dict_get_item(activation_node, "activation-record");
	if (!record) {
		return IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
	}

	*activation_record = plist_copy(record);

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

static void idevice_activation_response_add_field(idevice_activation_response_t response, const char* key, const char* value, int required_input)
{
	plist_dict_set_item(response->fields, key, plist_new_string(value));
	if (required_input) {
		plist_dict_set_item(response->fields_require_input, key, plist_new_bool(1));
	}
}

static idevice_activation_error_t idevice_activation_parse_buddyml_response(idevice_activation_response_t response)
{
	idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;
	xmlDocPtr doc = NULL;
	xmlXPathContextPtr context = NULL;
	xmlXPathObjectPtr xpath_result = NULL;
	int i = 0;

	if (!response->content_type == IDEVICE_ACTIVATION_CONTENT_TYPE_BUDDYML)
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
	xpath_result = xmlXPathEvalExpression((const xmlChar*) "/xmlui/page/tableView/section[@footer and not(@footerLinkURL)]/@footer", context);
	if (!xpath_result) {
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

			idevice_activation_response_add_field(response, (const char*) id, "", 1);

			xmlChar* label = xmlGetProp(xpath_result->nodesetval->nodeTab[i], (const xmlChar*) "label");
			if (label) {
				plist_dict_set_item(response->labels, (const char*)id, plist_new_string((const char*) label));
				xmlFree(label);
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
					(const char*) xpath_result->nodesetval->nodeTab[i]->name, (const char*) content, 0);
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

	if (!response->content_type == IDEVICE_ACTIVATION_CONTENT_TYPE_HTML)
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
		plist_t activation_node = NULL;
		xmlBufferPtr plistNodeBuffer = xmlBufferCreate();
		if (htmlNodeDump(plistNodeBuffer, doc, xpath_result->nodesetval->nodeTab[0]) == -1) {
			result = IDEVICE_ACTIVATION_E_HTML_PARSING_ERROR;
			goto local_cleanup;
		}

		result = idevice_activation_activation_node_from_plist_xml(
			(const char*) plistNodeBuffer->content, plistNodeBuffer->use, &activation_node);
		if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
			response->has_errors = 1;
			result = IDEVICE_ACTIVATION_E_SUCCESS;
			goto local_cleanup;
		}

		// check for ack-received
		plist_t ack_received = plist_dict_get_item(activation_node, "ack-received");
		if (ack_received) {
			uint8_t val = 0;
			plist_get_bool_val(ack_received, &val);
			if (val) {
				response->is_activation_ack = 1;
			} else {
				result = IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR;
			}

			goto local_cleanup;
		}

		// try to retrieve the activation record
		result = idevice_activation_activation_record_from_activation_node(activation_node, &response->activation_record);
		if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
			goto local_cleanup;
		}

	local_cleanup:
		if (plistNodeBuffer)
			xmlBufferFree(plistNodeBuffer);
		if (activation_node)
			plist_free(activation_node);
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
			plist_t activation_node = NULL;
			idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;

			result = idevice_activation_activation_node_from_plist_xml(response->raw_content, response->raw_content_size, &activation_node);
			if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
				return result;
			}

			result = idevice_activation_activation_record_from_activation_node(activation_node, &response->activation_record);
			plist_free(activation_node);
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
		response->raw_content = realloc(response->raw_content, response->raw_content_size + total);
		memcpy(response->raw_content + response->raw_content_size, data, total);
		response->raw_content_size += total;
	}

	return total;
}

static size_t idevice_activation_header_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
	idevice_activation_response_t response = (idevice_activation_response_t)userdata;
	const size_t total = size * nmemb;
	if (total != 0) {
		if (strstr(data, "Content-Type: text/xml") != NULL) {
			response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_PLIST;
		} else if (strstr(data, "Content-Type: application/x-buddyml") != NULL) {
			response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_BUDDYML;
		} else if (strstr(data, "Content-Type: text/html") != NULL) {
			response->content_type = IDEVICE_ACTIVATION_CONTENT_TYPE_HTML;
		}
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
	uint32_t size = 0;

	if (!xmlplist && !*xmlplist)
		return -1;

	char* start = strstr(*xmlplist, "<plist version=\"1.0\">\n");
	if (start == NULL) {
		return -1;
	}

	char* stop = strstr(*xmlplist, "\n</plist>");
	if (stop == NULL) {
		return -1;
	}

	start += strlen("<plist version=\"1.0\">\n");
	size = stop - start;
	char* stripped = malloc(size + 1);
	memset(stripped, '\0', size + 1);
	memcpy(stripped, start, size);
	free(*xmlplist);
	*xmlplist = stripped;
	stripped = NULL;

	return 0;
}

idevice_activation_error_t idevice_activation_request_new(idevice_activation_client_type_t client_type, idevice_activation_request_t* request)
{
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

idevice_activation_error_t idevice_activation_request_new_from_lockdownd(idevice_activation_client_type_t client_type, lockdownd_client_t lockdown, idevice_activation_request** request)
{
	// check arguments
	if (!lockdown) {
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
	}

	plist_t node = NULL;
	plist_t fields = plist_new_dict();

	// add InStoreActivation
	plist_dict_set_item(fields, "InStoreActivation", plist_new_string("false"));

	// add AppleSerialNumber
	if ((lockdownd_get_value(lockdown, NULL, "SerialNumber", &node) != LOCKDOWN_E_SUCCESS) || !node || (plist_get_node_type(node) != PLIST_STRING)) {
		fprintf(stderr, "%s: Unable to get SerialNumber from lockdownd\n", __func__);
		plist_free(fields);
		return IDEVICE_ACTIVATION_E_INCOMPLETE_INFO;
	} else {
		plist_dict_set_item(fields, "AppleSerialNumber", plist_copy(node));
	}
	if (node) {
		plist_free(node);
		node = NULL;
	}

	// add IMEI
	if ((lockdownd_get_value(lockdown, NULL, "InternationalMobileEquipmentIdentity", &node) != LOCKDOWN_E_SUCCESS) || !node || (plist_get_node_type(node) != PLIST_STRING)) {
		fprintf(stderr, "%s: Unable to get IMEI from lockdownd\n", __func__);
	} else {
		plist_dict_set_item(fields, "IMEI", plist_copy(node));
	}
	if (node) {
		plist_free(node);
		node = NULL;
	}

	// add MEID
	if ((lockdownd_get_value(lockdown, NULL, "MobileEquipmentIdentifier", &node) != LOCKDOWN_E_SUCCESS) || !node || (plist_get_node_type(node) != PLIST_STRING)) {
		fprintf(stderr, "%s: Unable to get MEID from lockdownd\n", __func__);
	} else {
		plist_dict_set_item(fields, "MEID", plist_copy(node));
	}
	if (node) {
		plist_free(node);
		node = NULL;
	}

	// add IMSI
	if ((lockdownd_get_value(lockdown, NULL, "InternationalMobileSubscriberIdentity", &node) != LOCKDOWN_E_SUCCESS) || !node || (plist_get_node_type(node) != PLIST_STRING)) {
		fprintf(stderr, "%s: Unable to get IMSI from lockdownd\n", __func__);
	} else {
		plist_dict_set_item(fields, "IMSI", plist_copy(node));
	}
	if (node) {
		plist_free(node);
		node = NULL;
	}

	// add ICCID
	if ((lockdownd_get_value(lockdown, NULL, "IntegratedCircuitCardIdentity", &node) != LOCKDOWN_E_SUCCESS) || !node || (plist_get_node_type(node) != PLIST_STRING)) {
		fprintf(stderr, "%s: Unable to get ICCID from lockdownd\n", __func__);
	} else {
		plist_dict_set_item(fields, "ICCID", plist_copy(node));
	}
	if (node) {
		plist_free(node);
		node = NULL;
	}

	// add activation-info
	if ((lockdownd_get_value(lockdown, NULL, "ActivationInfo", &node) != LOCKDOWN_E_SUCCESS) || !node || (plist_get_node_type(node) != PLIST_DICT)) {
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

void idevice_activation_request_free(idevice_activation_request_t request)
{
	if (!request)
		return;

	if (request->fields)
		plist_free(request->fields);

	free(request);
}

void idevice_activation_request_get_fields(idevice_activation_request_t request, plist_t* fields)
{
	*fields = plist_copy(request->fields);
}

void idevice_activation_request_set_fields(idevice_activation_request_t request, plist_t fields)
{
	if (!fields)
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

void idevice_activation_request_set_fields_from_response(idevice_activation_request_t request, const idevice_activation_response_t response)
{
	plist_t response_fields = NULL;
	idevice_activation_response_get_fields(response, &response_fields);
	if (response_fields) {
		idevice_activation_request_set_fields(request, response_fields);
		free(response_fields);
	}
}

void idevice_activation_request_set_field(idevice_activation_request_t request, const char* key, const char* value)
{
	plist_dict_set_item(request->fields, key, plist_new_string(value));
}

void idevice_activation_request_get_field(idevice_activation_request_t request, const char* key, char** value)
{
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

void idevice_activation_request_get_url(idevice_activation_request_t request, const char** url)
{
	*url = request->url;
}

void idevice_activation_request_set_url(idevice_activation_request_t request, const char* url)
{
	if (request->url) {
		free(request->url);
	}

	request->url = strdup(url);
}

idevice_activation_error_t idevice_activation_response_new(idevice_activation_response_t* response)
{
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
	tmp_response->fields = plist_new_dict();
	tmp_response->fields_require_input = plist_new_dict();
	tmp_response->labels = plist_new_dict();
	tmp_response->is_activation_ack = 0;
	tmp_response->is_auth_required = 0;
	tmp_response->has_errors = 0;
	*response = tmp_response;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

idevice_activation_error_t idevice_activation_response_new_from_html(const char* content, idevice_activation_response_t* response)
{
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

idevice_activation_error_t idevice_activation_response_to_buffer(idevice_activation_response_t response, char** buffer, size_t* size)
{
	char* tmp_buffer = (char*) malloc(sizeof(char) * response->raw_content_size);
	if (!tmp_buffer) {
		return IDEVICE_ACTIVATION_E_OUT_OF_MEMORY;
	}

	memcpy(tmp_buffer, response->raw_content, response->raw_content_size);

	*buffer = tmp_buffer;
	*size = response->raw_content_size;

	return IDEVICE_ACTIVATION_E_SUCCESS;
}

void idevice_activation_response_free(idevice_activation_response_t response)
{
	if (response->raw_content)
		free(response->raw_content);
	if (response->title)
		free(response->title);
	if (response->description)
		free(response->description);
	if (response->activation_record)
		plist_free(response->activation_record);
	if (response->fields)
		plist_free(response->fields);
	if (response->fields_require_input)
		plist_free(response->fields_require_input);
	if (response->labels)
		plist_free(response->labels);
	free(response);
}

void idevice_activation_response_get_field(idevice_activation_response_t response, const char* key, char** value)
{
	*value = NULL;
	plist_t item = plist_dict_get_item(response->fields, key);

	if (item && plist_get_node_type(item) == PLIST_STRING) {
		plist_get_string_val(item, value);
	}
}

void idevice_activation_response_get_fields(idevice_activation_response_t response, plist_t* fields)
{
	*fields = plist_copy(response->fields);
}

void idevice_activation_response_get_label(idevice_activation_response_t response, const char* key, char** value)
{
	*value = NULL;
	plist_t item = plist_dict_get_item(response->labels, key);
	if (item) {
		plist_get_string_val(item, value);
	}
}

void idevice_activation_response_get_title(idevice_activation_response_t response, const char** title)
{
	*title = response->title;
}

void idevice_activation_response_get_description(idevice_activation_response_t response, const char** description)
{
	*description = response->description;
}

void idevice_activation_response_get_activation_record(idevice_activation_response_t response, plist_t* activation_record)
{
	if (response->activation_record) {
		*activation_record = plist_copy(response->activation_record);
	} else {
		*activation_record = NULL;
	}
}

int idevice_activation_response_is_activation_acknowledged(idevice_activation_response_t response)
{
	return response->is_activation_ack;
}

int idevice_activation_response_is_authentication_required(idevice_activation_response_t response)
{
	return response->is_auth_required;
}

int idevice_activation_response_field_requires_input(idevice_activation_response_t response, const char* key)
{
	if (plist_dict_get_item(response->fields_require_input, key)) {
		return 1;
	}
	return 0;
}

int idevice_activation_response_has_errors(idevice_activation_response_t response)
{
	return response->has_errors;
}

idevice_activation_error_t idevice_activation_send_request(idevice_activation_request_t request, idevice_activation_response_t* response)
{
	idevice_activation_error_t result = IDEVICE_ACTIVATION_E_SUCCESS;

	// check arguments
	if (!request) {
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
	}

	plist_dict_iter iter = NULL;
	plist_dict_new_iter(request->fields, &iter);
	if (!iter) {
		return IDEVICE_ACTIVATION_E_INTERNAL_ERROR;
		goto cleanup;
	}

	curl_global_init(CURL_GLOBAL_ALL);
	CURL* handle = curl_easy_init();
	struct curl_httppost* form = NULL;

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

					if (svalue)
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
						if (postdata)
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

					if (svalue)
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

	// enable communication debugging
	if (debug_level > 0) {
		curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
	}

	curl_easy_perform(handle);

	if (debug_level > 0) {
		fprintf(stderr, "%*s\n", (int)tmp_response->raw_content_size, tmp_response->raw_content);
	}

	result = idevice_activation_parse_raw_response(tmp_response);
	if (result != IDEVICE_ACTIVATION_E_SUCCESS) {
		goto cleanup;
	}

	*response = tmp_response;

	result = IDEVICE_ACTIVATION_E_SUCCESS;

cleanup:
	if (iter)
		free(iter);
	if (form)
		curl_formfree(form);
	if (handle)
		curl_easy_cleanup(handle);

	curl_global_cleanup();

	return result;
}
