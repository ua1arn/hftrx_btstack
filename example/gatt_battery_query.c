/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "gatt_battery_query.c"

// *****************************************************************************
/* EXAMPLE_START(gatt_battery_query): GATT Battery Service Client
 *
 * @text This example demonstrates how to use the GATT Battery Service client to 
 * receive battery level information. The client supports querying of multiple 
 * battery services instances of on the remote device. 
 * The example scans for remote devices and connects to the first found device
 * and starts the battery service client.
 */
// *****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"

// gatt_battery_query.gatt contains the declaration of the provided GATT Services + Characteristics
// gatt_battery_query.h    contains the binary representation of gatt_battery_query.gatt
// it is generated by the build system by calling: $BTSTACK_ROOT/tool/compile_gatt.py gatt_battery_query.gatt gatt_battery_query.h
// it needs to be regenerated when the GATT Database declared in gatt_battery_query.gatt file is modified
#include "gatt_battery_query.h"

typedef struct advertising_report {
    uint8_t   type;
    uint8_t   event_type;
    uint8_t   address_type;
    bd_addr_t address;
    uint8_t   rssi;
    uint8_t   length;
    const uint8_t * data;
} advertising_report_t;

static enum {
    APP_STATE_IDLE,
    APP_STATE_W4_SCAN_RESULT,
    APP_STATE_W4_CONNECT,
    APP_STATE_CONNECTED
} app_state;

static int blacklist_index = 0;
static bd_addr_t blacklist[20];
static advertising_report_t report;

static hci_con_handle_t connection_handle;
static uint16_t battery_service_cid;

static bd_addr_t cmdline_addr;
static int cmdline_addr_found = 0;

static btstack_packet_callback_registration_t hci_event_callback_registration;

/* @section Main Application Setup
 *
 * @text The Listing MainConfiguration shows how to setup Battery Service client. 
 * Besides calling init() method for each service, you'll also need to register HCI packet handler 
 * to handle advertisements, as well as connect and disconect events.
 *
 * @text Handling of GATT Battery Service events will be later delegated to a sepparate packet 
 * handler, i.e. gatt_client_event_handler.
 *
 * @note There are two additional files associated with this client to allow a remote device to query out GATT database:
 * - gatt_battary_query.gatt - contains the declaration of the provided GATT Services and Characteristics.
 * - gatt_battary_query.h    - contains the binary representation of gatt_battary_query.gatt. 
 * 
 * gatt_battary_query.h is generated by the build system by calling: 
 * $BTSTACK_ROOT/tool/compile_gatt.py gatt_battary_query.gatt gatt_battary_query.h
 * This file needs to be regenerated when the GATT Database declared in gatt_battary_query.gatt file is modified.
 */

/* LISTING_START(MainConfiguration): Setup Device Battery Client service */
static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void gatt_client_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void battery_service_client_setup(void){
    // Init L2CAP
    l2cap_init();

    // Setup ATT server - only needed if LE Peripheral does ATT queries on its own, e.g. Android phones
    att_server_init(profile_data, NULL, NULL);    

    // GATT Client setup
    gatt_client_init();
    // Device Information Service Client setup
    battery_service_client_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);
} 
/* LISTING_END */

static int blacklist_size(void){
    return sizeof(blacklist) / sizeof(bd_addr_t);
}

static int blacklist_contains(bd_addr_t addr){
    int i;
    for (i=0; i<blacklist_size(); i++){
        if (bd_addr_cmp(addr, blacklist[i]) == 0) return 1;
    }
    return 0;
}

static void add_to_blacklist(bd_addr_t addr){
    printf("%s added to blacklist (no battery service found).\n", bd_addr_to_str(addr));
    bd_addr_copy(blacklist[blacklist_index], addr);
    blacklist_index = (blacklist_index + 1) % blacklist_size();
}

static void dump_advertising_report(uint8_t *packet){
    bd_addr_t address;
    gap_event_advertising_report_get_address(packet, address);

    printf("    * adv. event: evt-type %u, addr-type %u, addr %s, rssi %u, length adv %u, data: ", 
        gap_event_advertising_report_get_advertising_event_type(packet),
        gap_event_advertising_report_get_address_type(packet), 
        bd_addr_to_str(address), 
        gap_event_advertising_report_get_rssi(packet), 
        gap_event_advertising_report_get_data_length(packet));
    printf_hexdump(gap_event_advertising_report_get_data(packet), gap_event_advertising_report_get_data_length(packet));
    
}

/* LISTING_START(packetHandler): Packet Handler */
static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    /* LISTING_PAUSE */
    UNUSED(channel);
    UNUSED(size);

    /* LISTING_RESUME */

    if (packet_type != HCI_EVENT_PACKET){
        return;  
    } 

    switch (hci_event_packet_get_type(packet)) {
        /* LISTING_PAUSE */
        
        case BTSTACK_EVENT_STATE:
            // BTstack activated, get started
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            if (cmdline_addr_found){
                printf("Connect to %s\n", bd_addr_to_str(cmdline_addr));
                app_state = APP_STATE_W4_CONNECT;
                gap_connect(cmdline_addr, 0);
                break;
            }
            printf("Start scanning!\n");
            app_state = APP_STATE_W4_SCAN_RESULT;
            gap_set_scan_parameters(0,0x0030, 0x0030);
            gap_start_scan();
            break;

        case GAP_EVENT_ADVERTISING_REPORT:
            if (app_state != APP_STATE_W4_SCAN_RESULT) return;

            gap_event_advertising_report_get_address(packet, report.address);
            report.address_type = gap_event_advertising_report_get_address_type(packet);
            if (blacklist_contains(report.address)) {
                break;
            }
            dump_advertising_report(packet);

            // stop scanning, and connect to the device
            app_state = APP_STATE_W4_CONNECT;
            gap_stop_scan();
            printf("Stop scan. Connect to device with addr %s.\n", bd_addr_to_str(report.address));
            gap_connect(report.address,report.address_type);
            break;

        /* LISTING_RESUME */
        case HCI_EVENT_META_GAP:
            // Wait for connection complete
            if (hci_event_gap_meta_get_subevent_code(packet) !=  GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            
            /* LISTING_PAUSE */
            if (app_state != APP_STATE_W4_CONNECT) return;
            
            /* LISTING_RESUME */
            // Get connection handle from event
            connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            
            // Connect to remote Battery Service. 
            // On succesful connection, the client tries to register for notifications. If notifications 
            // are not supported by remote Battery Service, the client will automatically poll the battery level - here every 2 seconds.
            // If poll_interval_ms is 0, polling is disabled, and only notifications will be received (for manual polling, 
            // see battery_service_client.h).
            // All GATT Battery Service events are handled by the gatt_client_event_handler.
            (void) battery_service_client_connect(connection_handle, gatt_client_event_handler, 2000, &battery_service_cid);

            app_state = APP_STATE_CONNECTED;
            printf("Battery service connected.\n");
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            connection_handle = HCI_CON_HANDLE_INVALID;
            // Disconnect battery service
            battery_service_client_disconnect(battery_service_cid);
            
            /* LISTING_PAUSE */
            if (cmdline_addr_found){
                printf("Disconnected %s\n", bd_addr_to_str(cmdline_addr));
                return;
            }

            /* LISTING_RESUME */
            printf("Disconnected %s\n", bd_addr_to_str(report.address));
            printf("Restart scan.\n");
            app_state = APP_STATE_W4_SCAN_RESULT;
            gap_start_scan();
            break;
        default:
            break;
    }
}
/* LISTING_END */

/* LISTING_START(gatt_client_event_handler): GATT Client Event Handler */
// The gatt_client_event_handler receives following events from remote device:
//  - GATTSERVICE_SUBEVENT_BATTERY_SERVICE_CONNECTED
//  - GATTSERVICE_SUBEVENT_BATTERY_SERVICE_LEVEL     
// 
static void gatt_client_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    /* LISTING_PAUSE */
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    /* LISTING_RESUME */
    uint8_t status;
    uint8_t att_status;

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META){
        return;
    }
    
    switch (hci_event_gattservice_meta_get_subevent_code(packet)){
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_CONNECTED:
            status = gattservice_subevent_battery_service_connected_get_status(packet);
            switch (status){
                case ERROR_CODE_SUCCESS:
                    printf("Battery service client connected, found %d services, poll bitmap 0x%02x\n", 
                        gattservice_subevent_battery_service_connected_get_num_instances(packet),
                        gattservice_subevent_battery_service_connected_get_poll_bitmap(packet));
                        battery_service_client_read_battery_level(battery_service_cid, 0);
                    break;
                default:
                    printf("Battery service client connection failed, status 0x%02x.\n", status);
                    add_to_blacklist(report.address);
                    gap_disconnect(connection_handle);
                    break;
            }
            break;

        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_LEVEL:
            att_status = gattservice_subevent_battery_service_level_get_att_status(packet);
            if (att_status != ATT_ERROR_SUCCESS){
                printf("Battery level read failed, ATT Error 0x%02x\n", att_status);
            } else {
                printf("Service index: %d, Battery level: %d\n", 
                    gattservice_subevent_battery_service_level_get_sevice_index(packet), 
                    gattservice_subevent_battery_service_level_get_level(packet));
                    
            }
            break;

        default:
            break;
    }
}
 /* LISTING_END */

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    // parse address if command line arguments are provided
    int arg;
    cmdline_addr_found = 0;
    
    for (arg = 1; arg < argc; arg++) {
        if(!strcmp(argv[arg], "-a") || !strcmp(argv[arg], "--address")){
            if (arg + 1 < argc) {
                arg++;
                cmdline_addr_found = sscanf_bd_addr(argv[arg], cmdline_addr);
            }
            if (!cmdline_addr_found) {
                fprintf(stderr, "\nUsage: %s [-a|--address aa:bb:cc:dd:ee:ff]\n", argv[0]);
                fprintf(stderr, "If no argument is provided, %s will start scanning and connect to the first found device.\n"
                                "To connect to a specific device use argument [-a].\n\n", argv[0]);
                return 1;
            }
        }
    }
    if (!cmdline_addr_found) {
        fprintf(stderr, "No specific address specified or found; start scanning for any advertiser.\n");
    }
    
    battery_service_client_setup();

    app_state = APP_STATE_IDLE;

    // turn on!
    hci_power_control(HCI_POWER_ON);

    return 0;
}

/* EXAMPLE_END */


