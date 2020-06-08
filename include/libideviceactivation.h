/**
 * @file libideviceactivation.h
 * @brief Manage device activation process.
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

#ifndef LIBIDEVICEACTIVATION_H
#define LIBIDEVICEACTIVATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libimobiledevice/lockdown.h>
#include <plist/plist.h>

typedef enum {
	IDEVICE_ACTIVATION_E_SUCCESS                =  0,
	IDEVICE_ACTIVATION_E_INCOMPLETE_INFO        = -1,
	IDEVICE_ACTIVATION_E_OUT_OF_MEMORY          = -2,
	IDEVICE_ACTIVATION_E_UNKNOWN_CONTENT_TYPE   = -3,
	IDEVICE_ACTIVATION_E_BUDDYML_PARSING_ERROR  = -4,
	IDEVICE_ACTIVATION_E_PLIST_PARSING_ERROR    = -5,
	IDEVICE_ACTIVATION_E_HTML_PARSING_ERROR     = -6,
	IDEVICE_ACTIVATION_E_UNSUPPORTED_FIELD_TYPE = -7,
	IDEVICE_ACTIVATION_E_INTERNAL_ERROR         = -255
} idevice_activation_error_t;

typedef enum {
	IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION,
	IDEVICE_ACTIVATION_CLIENT_ITUNES
} idevice_activation_client_type_t;

typedef struct idevice_activation_request_private idevice_activation_request;
typedef idevice_activation_request* idevice_activation_request_t;
typedef struct idevice_activation_response_private idevice_activation_response;
typedef idevice_activation_response* idevice_activation_response_t;

/* Interface */

void idevice_activation_set_debug_level(int level);

idevice_activation_error_t idevice_activation_request_new(idevice_activation_client_type_t activation_type, idevice_activation_request_t* request);
idevice_activation_error_t idevice_activation_request_new_from_lockdownd(idevice_activation_client_type_t activation_type, lockdownd_client_t lockdown, idevice_activation_request_t* request);
idevice_activation_error_t idevice_activation_drm_handshake_request_new(idevice_activation_client_type_t client_type, idevice_activation_request_t* request);
void idevice_activation_request_free(idevice_activation_request_t request);

void idevice_activation_request_get_fields(idevice_activation_request_t request, plist_t* fields);
void idevice_activation_request_set_fields(idevice_activation_request_t request, plist_t fields);
void idevice_activation_request_set_fields_from_response(idevice_activation_request_t request, const idevice_activation_response_t response);
void idevice_activation_request_set_field(idevice_activation_request_t request, const char* key, const char* value);
void idevice_activation_request_get_field(idevice_activation_request_t request, const char* key, char** value);

void idevice_activation_request_get_url(idevice_activation_request_t request, const char** url);
void idevice_activation_request_set_url(idevice_activation_request_t request, const char* url);

idevice_activation_error_t idevice_activation_response_new(idevice_activation_response_t* response);
idevice_activation_error_t idevice_activation_response_new_from_html(const char* content, idevice_activation_response_t* response);
idevice_activation_error_t idevice_activation_response_to_buffer(idevice_activation_response_t response, char** buffer, size_t* size);
void idevice_activation_response_free(idevice_activation_response_t response);

void idevice_activation_response_get_field(idevice_activation_response_t response, const char* key, char** value);
void idevice_activation_response_get_fields(idevice_activation_response_t response, plist_t* fields);
void idevice_activation_response_get_label(idevice_activation_response_t response, const char* key, char** value);
void idevice_activation_response_get_placeholder(idevice_activation_response_t response, const char* key, char **value);

void idevice_activation_response_get_title(idevice_activation_response_t response, const char** title);
void idevice_activation_response_get_description(idevice_activation_response_t response, const char** description);
void idevice_activation_response_get_activation_record(idevice_activation_response_t response, plist_t* activation_record);
void idevice_activation_response_get_headers(idevice_activation_response_t response, plist_t* headers);

int idevice_activation_response_is_activation_acknowledged(idevice_activation_response_t response);
int idevice_activation_response_is_authentication_required(idevice_activation_response_t response);
int idevice_activation_response_field_requires_input(idevice_activation_response_t response, const char* key);
int idevice_activation_response_field_secure_input(idevice_activation_response_t response, const char* key);
int idevice_activation_response_has_errors(idevice_activation_response_t response);

idevice_activation_error_t idevice_activation_send_request(idevice_activation_request_t request, idevice_activation_response_t* response);

#ifdef __cplusplus
}
#endif

#endif
