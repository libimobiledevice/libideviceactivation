/*
 * ideviceactivation.c
 * A command line tool to handle the activation process
 *
 * Copyright (c) 2016-2017 Nikias Bassen, All Rights Reserved.
 * Copyright (c) 2014-2015 Martin Szulecki, All Rights Reserved.
 * Copyright (c) 2011-2015 Mirell Development, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <plist/plist.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/mobileactivation.h>
#include <libideviceactivation.h>

static void print_usage(int argc, char **argv)
{
	char *name = NULL;
	
	name = strrchr(argv[0], '/');
	printf("Usage: %s COMMAND [OPTIONS]\n", (name ? name + 1: argv[0]));
	printf("Activate or deactivate a device.\n\n");
	printf("Where COMMAND is one of:\n");
	printf("  activate\t\tattempt to activate the device\n");
	printf("  deactivate\t\tdeactivate the device\n");
	printf("  state\t\t\tquery device about its activation state\n");
	printf("\nThe following OPTIONS are accepted:\n");
	printf("  -d, --debug\t\tenable communication debugging\n");
	printf("  -u, --udid UDID\ttarget specific device by its 40-digit device UDID\n");
	printf("  -s, --service URL\tuse activation webservice at URL instead of default\n");
	printf("  -v, --version\t\tprint version information and exit\n");
	printf("  -h, --help\t\tprints usage information\n");
	printf("\n");
	printf("Homepage: <http://libimobiledevice.org>\n");
}

int main(int argc, char *argv[])
{
	idevice_t device = NULL;
	idevice_error_t ret = IDEVICE_E_UNKNOWN_ERROR;
	lockdownd_client_t lockdown = NULL;
	mobileactivation_client_t ma = NULL;
	idevice_activation_request_t request = NULL;
	idevice_activation_response_t response = NULL;
	const char* response_title = NULL;
	const char* response_description = NULL;
	char* field_key = NULL;
	char* field_label = NULL;
	char input[1024];
	plist_t fields = NULL;
	plist_dict_iter iter = NULL;
	plist_t record = NULL;
	char *udid = NULL;
	char *signing_service_url = NULL;
	int use_mobileactivation = 0;
	int session_mode = 0;
	int i;
	int result = EXIT_FAILURE;

	typedef enum {
		OP_NONE = 0, OP_ACTIVATE, OP_DEACTIVATE, OP_GETSTATE
	} op_t;
	op_t op = OP_NONE;

	/* parse cmdline args */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
			idevice_set_debug_level(1);
			idevice_activation_set_debug_level(1);
			continue;
		}
		else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--udid")) {
			i++;
			if (!argv[i] || (strlen(argv[i]) != 40)) {
				print_usage(argc, argv);
				return EXIT_FAILURE;
			}
			udid = argv[i];
			continue;
		}
		else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--service")) {
			i++;
			if (!argv[i]) {
				print_usage(argc, argv);
				return EXIT_FAILURE;
			}
			signing_service_url = argv[i];
			continue;
		}
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argc, argv);
			return EXIT_SUCCESS;
		}
		else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			printf("ideviceactivation %s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;
		}
		else if (!strcmp(argv[i], "activate")) {
			op = OP_ACTIVATE;
			continue;
		}
		else if (!strcmp(argv[i], "deactivate")) {
			op = OP_DEACTIVATE;
			continue;
		}
		else if (!strcmp(argv[i], "state")) {
			op = OP_GETSTATE;
			continue;
		}
		else {
			print_usage(argc, argv);
			return EXIT_SUCCESS;
		}
	}

	if (op == OP_NONE) {
		print_usage(argc, argv);
		return EXIT_FAILURE;
	}

	if (udid) {
		ret = idevice_new(&device, udid);
		if (ret != IDEVICE_E_SUCCESS) {
			printf("No device found with UDID %s, is it plugged in?\n", udid);
			return EXIT_FAILURE;
		}
	}
	else
	{
		ret = idevice_new(&device, NULL);
		if (ret != IDEVICE_E_SUCCESS) {
			printf("No device found, is it plugged in?\n");
			return EXIT_FAILURE;
		}
	}

	if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdown, "ideviceactivation")) {
		fprintf(stderr, "Failed to connect to lockdownd\n");
		result = EXIT_FAILURE;
		goto cleanup;
	}

	plist_t p_version = NULL;
	uint32_t product_version = 0;
	if (lockdownd_get_value(lockdown, NULL, "ProductVersion", &p_version) == LOCKDOWN_E_SUCCESS) {
		int vers[3] = {0, 0, 0};
		char *s_version = NULL;
		plist_get_string_val(p_version, &s_version);
		if (s_version && sscanf(s_version, "%d.%d.%d", &vers[0], &vers[1], &vers[2]) >= 2) {
			product_version = ((vers[0] & 0xFF) << 16) | ((vers[1] & 0xFF) << 8) | (vers[2] & 0xFF);
		}
		free(s_version);
	}
	plist_free(p_version);

	if (op == OP_ACTIVATE && product_version >= 0x0A0200) {
		/* The activation server will not acknowledge the activation for iOS >= 10.2 anymore. Let's warn the user about this. */
		plist_t state = NULL;
		lockdownd_get_value(lockdown, NULL, "ActivationState", &state);
		if (state) {
			char *state_str = NULL;
			plist_get_string_val(state, &state_str);
			if (state_str && strcmp(state_str, "Unactivated") != 0) {
				printf("NOTE: This device appears to be already activated. The server might report an error 'Device Unknown' instead of acknowledging the activation.\n");
			}
			free(state_str);
			plist_free(state);
		}
	}

	// check if we should use the new mobileactivation service
	lockdownd_service_descriptor_t svc = NULL;
	if (lockdownd_start_service(lockdown, MOBILEACTIVATION_SERVICE_NAME, &svc) == LOCKDOWN_E_SUCCESS) {
		mobileactivation_error_t maerr = mobileactivation_client_new(device, svc, &ma);
		lockdownd_service_descriptor_free(svc);
		svc = NULL;
		if (maerr != MOBILEACTIVATION_E_SUCCESS) {
			fprintf(stderr, "Failed to connect to %s\n", MOBILEACTIVATION_SERVICE_NAME);
			result = EXIT_FAILURE;
			goto cleanup;
		}
		use_mobileactivation = 1;
	}

	switch (op) {
		case OP_DEACTIVATE:
			if (use_mobileactivation) {
				// deactivate device using mobileactivation
				if (MOBILEACTIVATION_E_SUCCESS != mobileactivation_deactivate(ma)) {
					fprintf(stderr, "Failed to deactivate device.\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}
				mobileactivation_client_free(ma);
				ma = NULL;
			} else {
				// deactivate device using lockdown
				if (LOCKDOWN_E_SUCCESS != lockdownd_deactivate(lockdown)) {
					fprintf(stderr, "Failed to deactivate device.\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}
			}

			result = EXIT_SUCCESS;
			printf("Successfully deactivated device.\n");
			break;
		case OP_ACTIVATE:
		default:
			if (use_mobileactivation) {
				// create activation request from mobileactivation
				plist_t ainfo = NULL;
				if ((product_version >= 0x0A0000) || (mobileactivation_create_activation_info(ma, &ainfo) != MOBILEACTIVATION_E_SUCCESS)) {
					session_mode = 1;
				}
				mobileactivation_client_free(ma);
				ma = NULL;
				if (session_mode) {
					/* first grab session blob from device required for drmHandshake */
					plist_t blob = NULL;
					if (mobileactivation_client_start_service(device, &ma, "ideviceactivation") != MOBILEACTIVATION_E_SUCCESS) {
						fprintf(stderr, "Failed to connect to %s\n", MOBILEACTIVATION_SERVICE_NAME);
						result = EXIT_FAILURE;
						goto cleanup;
					}
					if (mobileactivation_create_activation_session_info(ma, &blob) != MOBILEACTIVATION_E_SUCCESS) {
						fprintf(stderr, "Failed to get ActivationSessionInfo from mobileactivation\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}
					mobileactivation_client_free(ma);
					ma = NULL;

					/* create drmHandshake request with blob from device */
					if (idevice_activation_drm_handshake_request_new(IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION, &request) != IDEVICE_ACTIVATION_E_SUCCESS) {
						fprintf(stderr, "Failed to create drmHandshake request.\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}
					idevice_activation_request_set_fields(request, blob);
					plist_free(blob);

					/* send request to server and get response */
					idevice_activation_send_request(request, &response);
					plist_t handshake_response = NULL;
					idevice_activation_response_get_fields(response, &handshake_response);
					idevice_activation_response_free(response);
					response = NULL;

					/* use handshake response to get activation info from device */
					if (mobileactivation_client_start_service(device, &ma, "ideviceactivation") != MOBILEACTIVATION_E_SUCCESS) {
						fprintf(stderr, "Failed to connect to %s\n", MOBILEACTIVATION_SERVICE_NAME);
						result = EXIT_FAILURE;
						goto cleanup;
					}
					if ((mobileactivation_create_activation_info_with_session(ma, handshake_response, &ainfo) != MOBILEACTIVATION_E_SUCCESS) || !ainfo || (plist_get_node_type(ainfo) != PLIST_DICT)) {
						fprintf(stderr, "Failed to get ActivationInfo from mobileactivation\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}
					mobileactivation_client_free(ma);
					ma = NULL;
				} else if (!ainfo || plist_get_node_type(ainfo) != PLIST_DICT) {
					fprintf(stderr, "Failed to get ActivationInfo from mobileactivation\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}

				/* create activation request */
				if (idevice_activation_request_new(IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION, &request) != IDEVICE_ACTIVATION_E_SUCCESS) {
					fprintf(stderr, "Failed to create activation request.\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}

				/* add activation info to request */
				plist_t request_fields = plist_new_dict();
				plist_dict_set_item(request_fields, "activation-info", ainfo);
				idevice_activation_request_set_fields(request, request_fields);
			} else {
				// create activation request from lockdown
				if (idevice_activation_request_new_from_lockdownd(
					IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION, lockdown, &request) != IDEVICE_ACTIVATION_E_SUCCESS) {
					fprintf(stderr, "Failed to create activation request.\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}
			}
			lockdownd_client_free(lockdown);
			lockdown = NULL;

			if (request && signing_service_url) {
				idevice_activation_request_set_url(request, signing_service_url);
			}

			while(1) {
				if (idevice_activation_send_request(request, &response) != IDEVICE_ACTIVATION_E_SUCCESS) {
					fprintf(stderr, "Failed to send request or retrieve response.\n");
					// Here response might have some content that could't be correctly interpreted (parsed)
					// by the library. Printing out the content could help to identify the cause of the error.
					result = EXIT_FAILURE;
					goto cleanup;
				}

				if (idevice_activation_response_has_errors(response)) {
					fprintf(stderr, "Activation server reports errors.\n");

					idevice_activation_response_get_title(response, &response_title);
					if (response_title) {
						fprintf(stderr, "\t%s\n", response_title);
					}

					idevice_activation_response_get_description(response, &response_description);
					if (response_description) {
						fprintf(stderr, "\t%s\n", response_description);
					}
					result = EXIT_FAILURE;
					goto cleanup;
				}

				idevice_activation_response_get_activation_record(response, &record);

				if (record) {
					if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdown, "ideviceactivation")) {
						fprintf(stderr, "Failed to connect to lockdownd\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}
					if (use_mobileactivation) {
						svc = NULL;
						if (lockdownd_start_service(lockdown, MOBILEACTIVATION_SERVICE_NAME, &svc) != LOCKDOWN_E_SUCCESS) {
							fprintf(stderr, "Failed to start service %s\n", MOBILEACTIVATION_SERVICE_NAME);
							result = EXIT_FAILURE;
							goto cleanup;
						}
						mobileactivation_error_t maerr = mobileactivation_client_new(device, svc, &ma);
						lockdownd_service_descriptor_free(svc);
						svc = NULL;
						if (maerr != MOBILEACTIVATION_E_SUCCESS) {
							fprintf(stderr, "Failed to connect to %s\n", MOBILEACTIVATION_SERVICE_NAME);
							result = EXIT_FAILURE;
							goto cleanup;
						}

						if (session_mode) {
							plist_t headers = NULL;
							idevice_activation_response_get_headers(response, &headers);
							if (MOBILEACTIVATION_E_SUCCESS != mobileactivation_activate_with_session(ma, record, headers)) {
								plist_free(headers);
								fprintf(stderr, "Failed to activate device with record.\n");
								result = EXIT_FAILURE;
								goto cleanup;
							}
							plist_free(headers);
						} else {
							if (MOBILEACTIVATION_E_SUCCESS != mobileactivation_activate(ma, record)) {
								fprintf(stderr, "Failed to activate device with record.\n");
								result = EXIT_FAILURE;
								goto cleanup;
							}
						}
					} else {
						// activate device using lockdown
						if (LOCKDOWN_E_SUCCESS != lockdownd_activate(lockdown, record)) {
							fprintf(stderr, "Failed to activate device with record.\n");
							result = EXIT_FAILURE;
							goto cleanup;
						}
					}

					// set ActivationStateAcknowledged if we succeeded
					if (LOCKDOWN_E_SUCCESS != lockdownd_set_value(lockdown, NULL, "ActivationStateAcknowledged", plist_new_bool(1))) {
						fprintf(stderr, "Failed to set ActivationStateAcknowledged on device.\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}
					break;
				} else {
					if (idevice_activation_response_is_activation_acknowledged(response)) {
						printf("Activation server reports that device is already activated.\n");
						result = EXIT_SUCCESS;
						goto cleanup;
					}

					idevice_activation_response_get_title(response, &response_title);
					if (response_title) {
						fprintf(stderr, "Server reports:\n%s\n", response_title);
					}

					idevice_activation_response_get_description(response, &response_description);
					if (response_description) {
						fprintf(stderr, "Server reports:\n%s\n", response_description);
					}

					idevice_activation_response_get_fields(response, &fields);
					if (!fields || plist_dict_get_size(fields) == 0) {
						// we have no activation record, no reported erros, no acknowledgment and no fields to send
						fprintf(stderr, "Unknown error.\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}

					plist_dict_new_iter(fields, &iter);
					if (!iter) {
						fprintf(stderr, "Unknown error.\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}

					idevice_activation_request_free(request);
					request = NULL;
					if (idevice_activation_request_new(
						IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION, &request) != IDEVICE_ACTIVATION_E_SUCCESS) {
						fprintf(stderr, "Could not create new request.\n");
						result = EXIT_FAILURE;
						goto cleanup;
					}

					idevice_activation_request_set_fields_from_response(request, response);

					do {
						field_key = NULL;
						plist_dict_next_item(fields, iter, &field_key, NULL);
						if (field_key) {
							if (idevice_activation_response_field_requires_input(response, field_key)) {
								idevice_activation_response_get_label(response, field_key, &field_label);
								printf("input %s: ", field_label ? field_label : field_key);
								fflush(stdin);
								scanf("%1023s", input);
								idevice_activation_request_set_field(request, field_key, input);
								if (field_label) {
									free(field_label);
									field_label = NULL;
								}
							}
						}
					} while(field_key);

					free(iter);
					iter = NULL;
					idevice_activation_response_free(response);
					response = NULL;
				}

			}

			result = EXIT_SUCCESS;
			printf("Successfully activated device.\n");
			break;
		case OP_GETSTATE: {
			plist_t state = NULL;
			if (use_mobileactivation) {
				mobileactivation_get_activation_state(ma, &state);
			} else {
				lockdownd_get_value(lockdown, NULL, "ActivationState", &state);
			}
			if (plist_get_node_type(state) == PLIST_STRING) {
				char *s_state = NULL;
				plist_get_string_val(state, &s_state);
				printf("ActivationState: %s\n", s_state);
				free(s_state);
			} else {
				printf("Error getting activation state.\n");
			}
			}
			break;
	}

cleanup:
	if (request)
		idevice_activation_request_free(request);

	if (response)
		idevice_activation_response_free(response);

	if (fields)
		plist_free(fields);

	if (field_label)
		free(field_label);

	if (iter)
		free(iter);

	if (record)
		plist_free(record);

	if (ma)
		mobileactivation_client_free(ma);

	if (lockdown)
		lockdownd_client_free(lockdown);

	if (device)
		idevice_free(device);

	return result;
}
