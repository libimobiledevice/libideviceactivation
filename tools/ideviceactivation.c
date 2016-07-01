/*
 * ideviceactivation.c
 * A command line tool to handle the activation process
 *
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
	int i;
	int result = EXIT_FAILURE;

	typedef enum {
		OP_NONE = 0, OP_ACTIVATE, OP_DEACTIVATE
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
		result = EXIT_FAILURE;
		goto cleanup;
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

				if ((mobileactivation_create_activation_info(ma, &ainfo) != MOBILEACTIVATION_E_SUCCESS) || !ainfo || (plist_get_node_type(ainfo) != PLIST_DICT)) {
					fprintf(stderr, "Failed to get ActivationInfo from mobileactivation\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}

				if (idevice_activation_request_new(IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION, &request) != IDEVICE_ACTIVATION_E_SUCCESS) {
					fprintf(stderr, "Failed to create activation request.\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}

				plist_t request_fields = plist_new_dict();
				plist_dict_set_item(request_fields, "activation-info", ainfo);

				idevice_activation_request_set_fields(request, request_fields);

				mobileactivation_client_free(ma);
				ma = NULL;
			} else {
				// create activation request from lockdown
				if (idevice_activation_request_new_from_lockdownd(
					IDEVICE_ACTIVATION_CLIENT_MOBILE_ACTIVATION, lockdown, &request) != IDEVICE_ACTIVATION_E_SUCCESS) {
					fprintf(stderr, "Failed to create activation request.\n");
					result = EXIT_FAILURE;
					goto cleanup;
				}
			}

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

				if (idevice_activation_response_is_activation_acknowledged(response)) {
					printf("Activation server reports that device is already activated.\n");
					result = EXIT_SUCCESS;
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
					if (use_mobileactivation) {
						svc = NULL;
						if (lockdownd_start_service(lockdown, MOBILEACTIVATION_SERVICE_NAME, &svc) == LOCKDOWN_E_SUCCESS) {
							mobileactivation_error_t maerr = mobileactivation_client_new(device, svc, &ma);
							lockdownd_service_descriptor_free(svc);
							if (maerr != MOBILEACTIVATION_E_SUCCESS) {
								fprintf(stderr, "Failed to connect to %s\n", MOBILEACTIVATION_SERVICE_NAME);
								result = EXIT_FAILURE;
								goto cleanup;
							}
						}

						if (MOBILEACTIVATION_E_SUCCESS != mobileactivation_activate(ma, record)) {
							fprintf(stderr, "Failed to activate device with record.\n");
							result = EXIT_FAILURE;
							goto cleanup;
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
