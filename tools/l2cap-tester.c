// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/l2cap.h"
#include "lib/mgmt.h"

#include "monitor/bt.h"
#include "emulator/bthost.h"
#include "emulator/hciemu.h"

#include "src/shared/tester.h"
#include "src/shared/mgmt.h"
#include "src/shared/util.h"

#include "tester.h"

struct test_data {
	const void *test_data;
	struct mgmt *mgmt;
	uint16_t mgmt_index;
	struct hciemu *hciemu;
	enum hciemu_type hciemu_type;
	unsigned int io_id;
	unsigned int err_io_id;
	uint16_t handle;
	uint16_t scid;
	uint16_t dcid;
	struct l2cap_options l2o;
	int sk;
	int sk2;
	bool host_disconnected;
	int step;
	struct tx_tstamp_data tx_ts;
};

struct l2cap_data {
	uint16_t client_psm;
	uint16_t server_psm;
	uint16_t cid;
	uint8_t mode;
	uint16_t mtu;
	uint16_t mps;
	uint16_t credits;
	int expect_err;
	int timeout;

	uint8_t send_cmd_code;
	const void *send_cmd;
	uint16_t send_cmd_len;
	uint8_t expect_cmd_code;
	const void *expect_cmd;
	uint16_t expect_cmd_len;

	uint16_t data_len;
	const void *read_data;
	const void *write_data;

	bool enable_ssp;
	uint8_t client_io_cap;
	int sec_level;
	bool reject_ssp;

	bool expect_pin;
	uint8_t pin_len;
	const void *pin;
	uint8_t client_pin_len;
	const void *client_pin;

	bool addr_type_avail;
	uint8_t addr_type;

	uint8_t *client_bdaddr;
	bool server_not_advertising;
	bool direct_advertising;
	bool close_1;
	bool defer;

	bool shut_sock_wr;

	/* Enable SO_TIMESTAMPING with these flags */
	uint32_t so_timestamping;

	/* Number of additional packets to send. */
	unsigned int repeat_send;

	/* Socket type (0 means SOCK_SEQPACKET) */
	int sock_type;
};

static void print_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_print("%s%s", prefix, str);
}

static void read_info_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct mgmt_rp_read_info *rp = param;
	char addr[18];
	uint16_t manufacturer;
	uint32_t supported_settings, current_settings;

	tester_print("Read Info callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	ba2str(&rp->bdaddr, addr);
	manufacturer = btohs(rp->manufacturer);
	supported_settings = btohl(rp->supported_settings);
	current_settings = btohl(rp->current_settings);

	tester_print("  Address: %s", addr);
	tester_print("  Version: 0x%02x", rp->version);
	tester_print("  Manufacturer: 0x%04x", manufacturer);
	tester_print("  Supported settings: 0x%08x", supported_settings);
	tester_print("  Current settings: 0x%08x", current_settings);
	tester_print("  Class: 0x%02x%02x%02x",
			rp->dev_class[2], rp->dev_class[1], rp->dev_class[0]);
	tester_print("  Name: %s", rp->name);
	tester_print("  Short name: %s", rp->short_name);

	if (strcmp(hciemu_get_address(data->hciemu), addr)) {
		tester_pre_setup_failed();
		return;
	}

	tester_pre_setup_complete();
}

static void index_added_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Added callback");
	tester_print("  Index: 0x%04x", index);

	data->mgmt_index = index;

	mgmt_send(data->mgmt, MGMT_OP_READ_INFO, data->mgmt_index, 0, NULL,
					read_info_callback, NULL, NULL);
}

static void index_removed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Removed callback");
	tester_print("  Index: 0x%04x", index);

	if (index != data->mgmt_index)
		return;

	mgmt_unregister_index(data->mgmt, data->mgmt_index);

	mgmt_unref(data->mgmt);
	data->mgmt = NULL;

	tester_post_teardown_complete();
}

static void read_index_list_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Read Index List callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	mgmt_register(data->mgmt, MGMT_EV_INDEX_ADDED, MGMT_INDEX_NONE,
					index_added_callback, NULL, NULL);

	mgmt_register(data->mgmt, MGMT_EV_INDEX_REMOVED, MGMT_INDEX_NONE,
					index_removed_callback, NULL, NULL);

	data->hciemu = hciemu_new(data->hciemu_type);
	if (!data->hciemu) {
		tester_warn("Failed to setup HCI emulation");
		tester_pre_setup_failed();
	}

	if (tester_use_debug())
		hciemu_set_debug(data->hciemu, print_debug, "hciemu: ", NULL);

	tester_print("New hciemu instance created");
}

static void test_pre_setup(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->mgmt = mgmt_new_default();
	if (!data->mgmt) {
		tester_warn("Failed to setup management interface");
		tester_pre_setup_failed();
		return;
	}

	if (tester_use_debug())
		mgmt_set_debug(data->mgmt, print_debug, "mgmt: ", NULL);

	mgmt_send(data->mgmt, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, 0, NULL,
					read_index_list_callback, NULL, NULL);
}

static void test_post_teardown(const void *test_data)
{
	struct test_data *data = tester_get_data();

	if (data->io_id > 0) {
		g_source_remove(data->io_id);
		data->io_id = 0;
	}

	if (data->err_io_id > 0) {
		g_source_remove(data->err_io_id);
		data->err_io_id = 0;
	}

	hciemu_unref(data->hciemu);
	data->hciemu = NULL;
}

static void test_data_free(void *test_data)
{
	struct test_data *data = test_data;

	free(data);
}

#define test_l2cap_bredr(name, data, setup, func) \
	do { \
		struct test_data *user; \
		user = malloc(sizeof(struct test_data)); \
		if (!user) \
			break; \
		user->hciemu_type = HCIEMU_TYPE_BREDR; \
		user->io_id = 0; \
		user->err_io_id = 0; \
		user->test_data = data; \
		tester_add_full(name, data, \
				test_pre_setup, setup, func, NULL, \
				test_post_teardown, 2, user, test_data_free); \
	} while (0)

#define test_l2cap_le(name, data, setup, func) \
	do { \
		struct test_data *user; \
		user = malloc(sizeof(struct test_data)); \
		if (!user) \
			break; \
		user->hciemu_type = HCIEMU_TYPE_LE; \
		user->io_id = 0; \
		user->err_io_id = 0; \
		user->test_data = data; \
		tester_add_full(name, data, \
				test_pre_setup, setup, func, NULL, \
				test_post_teardown, 2, user, test_data_free); \
	} while (0)

static uint8_t pair_device_pin[] = { 0x30, 0x30, 0x30, 0x30 }; /* "0000" */

static const struct l2cap_data client_connect_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
};

static const struct l2cap_data client_connect_close_test = {
	.client_psm = 0x1001,
};

static const struct l2cap_data client_connect_timeout_test = {
	.client_psm = 0x1001,
	.timeout = 1
};

static const struct l2cap_data client_connect_ssp_success_test_1 = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.enable_ssp = true,
};

static const struct l2cap_data client_connect_ssp_success_test_2 = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.enable_ssp = true,
	.sec_level  = BT_SECURITY_HIGH,
	.client_io_cap = 0x04,
};

static const struct l2cap_data client_connect_pin_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.sec_level  = BT_SECURITY_MEDIUM,
	.pin = pair_device_pin,
	.pin_len = sizeof(pair_device_pin),
	.client_pin = pair_device_pin,
	.client_pin_len = sizeof(pair_device_pin),
};

static uint8_t l2_data[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };

const uint8_t l2_data_32k[32768] = { [0 ... 4095] =  0x00,
				[4096 ... 8191] =  0x01,
				[8192 ... 12287] =  0x02,
				[12288 ... 16383] =  0x03,
				[16384 ... 20479] =  0x04,
				[20480 ... 24575] =  0x05,
				[24576 ... 28671] =  0x06,
				[28672 ... 32767] =  0x07,
};

static const struct l2cap_data client_connect_read_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.read_data = l2_data,
	.data_len = sizeof(l2_data),
};

static const struct l2cap_data client_connect_read_32k_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.read_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
};

static const struct l2cap_data client_connect_rx_timestamping_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.read_data = l2_data,
	.data_len = sizeof(l2_data),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_RX_SOFTWARE),
};

static const struct l2cap_data client_connect_rx_timestamping_32k_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.read_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_RX_SOFTWARE),
};

static const struct l2cap_data client_connect_write_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.write_data = l2_data,
	.data_len = sizeof(l2_data),
};

static const struct l2cap_data client_connect_write_32k_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.write_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
};

static const struct l2cap_data client_connect_tx_timestamping_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.write_data = l2_data,
	.data_len = sizeof(l2_data),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_OPT_ID |
					SOF_TIMESTAMPING_TX_SOFTWARE |
					SOF_TIMESTAMPING_TX_COMPLETION),
	.repeat_send = 2,
};

static const struct l2cap_data client_connect_stream_tx_timestamping_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.write_data = l2_data,
	.data_len = sizeof(l2_data),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_OPT_ID |
					SOF_TIMESTAMPING_TX_SOFTWARE |
					SOF_TIMESTAMPING_TX_COMPLETION),
	.repeat_send = 2,
	.sock_type = SOCK_STREAM,
};

static const struct l2cap_data client_connect_shut_wr_success_test = {
	.client_psm = 0x1001,
	.server_psm = 0x1001,
	.shut_sock_wr = true,
};

static const struct l2cap_data client_connect_nval_psm_test_1 = {
	.client_psm = 0x1001,
	.expect_err = ECONNREFUSED,
};

static const struct l2cap_data client_connect_nval_psm_test_2 = {
	.client_psm = 0x0001,
	.expect_err = ECONNREFUSED,
};

static const struct l2cap_data client_connect_nval_psm_test_3 = {
	.client_psm = 0x0001,
	.expect_err = ECONNREFUSED,
	.enable_ssp = true,
};

static const uint8_t l2cap_connect_req[] = { 0x01, 0x10, 0x41, 0x00 };

static const struct l2cap_data l2cap_server_success_test = {
	.server_psm = 0x1001,
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
};

static const struct l2cap_data l2cap_server_read_success_test = {
	.server_psm = 0x1001,
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
	.read_data = l2_data,
	.data_len = sizeof(l2_data),
};

static const struct l2cap_data l2cap_server_read_32k_success_test = {
	.server_psm = 0x1001,
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
	.read_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
};

static const struct l2cap_data l2cap_server_write_success_test = {
	.server_psm = 0x1001,
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
	.write_data = l2_data,
	.data_len = sizeof(l2_data),
};

static const struct l2cap_data l2cap_server_write_32k_success_test = {
	.server_psm = 0x1001,
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
	.write_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
};

static const uint8_t l2cap_sec_block_rsp[] = {	0x00, 0x00,	/* dcid */
						0x41, 0x00,	/* scid */
						0x03, 0x00,	/* Sec Block */
						0x00, 0x00	/* status */
					};

static const struct l2cap_data l2cap_server_sec_block_test = {
	.server_psm = 0x1001,
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
	.expect_cmd = l2cap_sec_block_rsp,
	.expect_cmd_len = sizeof(l2cap_sec_block_rsp),
	.enable_ssp = true,
};

static const uint8_t l2cap_nval_psm_rsp[] = {	0x00, 0x00,	/* dcid */
						0x41, 0x00,	/* scid */
						0x02, 0x00,	/* nval PSM */
						0x00, 0x00	/* status */
					};

static const struct l2cap_data l2cap_server_nval_psm_test = {
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_connect_req,
	.send_cmd_len = sizeof(l2cap_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_CONN_RSP,
	.expect_cmd = l2cap_nval_psm_rsp,
	.expect_cmd_len = sizeof(l2cap_nval_psm_rsp),
};

static const uint8_t l2cap_nval_conn_req[] = { 0x00 };
static const uint8_t l2cap_nval_pdu_rsp[] = { 0x00, 0x00 };

static const struct l2cap_data l2cap_server_nval_pdu_test1 = {
	.send_cmd_code = BT_L2CAP_PDU_CONN_REQ,
	.send_cmd = l2cap_nval_conn_req,
	.send_cmd_len = sizeof(l2cap_nval_conn_req),
	.expect_cmd_code = BT_L2CAP_PDU_CMD_REJECT,
	.expect_cmd = l2cap_nval_pdu_rsp,
	.expect_cmd_len = sizeof(l2cap_nval_pdu_rsp),
};

static const uint8_t l2cap_nval_dc_req[] = { 0x12, 0x34, 0x56, 0x78 };
static const uint8_t l2cap_nval_cid_rsp[] = { 0x02, 0x00,
						0x12, 0x34, 0x56, 0x78 };

static const struct l2cap_data l2cap_server_nval_cid_test1 = {
	.send_cmd_code = BT_L2CAP_PDU_DISCONN_REQ,
	.send_cmd = l2cap_nval_dc_req,
	.send_cmd_len = sizeof(l2cap_nval_dc_req),
	.expect_cmd_code = BT_L2CAP_PDU_CMD_REJECT,
	.expect_cmd = l2cap_nval_cid_rsp,
	.expect_cmd_len = sizeof(l2cap_nval_cid_rsp),
};

static const uint8_t l2cap_nval_cfg_req[] = { 0x12, 0x34, 0x00, 0x00 };
static const uint8_t l2cap_nval_cfg_rsp[] = { 0x02, 0x00,
						0x12, 0x34, 0x00, 0x00 };

static const struct l2cap_data l2cap_server_nval_cid_test2 = {
	.send_cmd_code = BT_L2CAP_PDU_CONFIG_REQ,
	.send_cmd = l2cap_nval_cfg_req,
	.send_cmd_len = sizeof(l2cap_nval_cfg_req),
	.expect_cmd_code = BT_L2CAP_PDU_CMD_REJECT,
	.expect_cmd = l2cap_nval_cfg_rsp,
	.expect_cmd_len = sizeof(l2cap_nval_cfg_rsp),
};

static const struct l2cap_data le_client_connect_success_test_1 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
};

static const struct l2cap_data le_client_connect_close_test_1 = {
	.client_psm = 0x0080,
};

static const struct l2cap_data le_client_connect_timeout_test_1 = {
	.client_psm = 0x0080,
	.timeout = 1,
};

static const struct l2cap_data le_client_connect_read_success_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.read_data = l2_data,
	.data_len = sizeof(l2_data),
};

static const struct l2cap_data le_client_connect_read_32k_success_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mtu = 672,
	.mps = 251,
	/* Given enough credits to complete the transfer without waiting for
	 * more credits.
	 * credits = round_up(data size / mtu) * round_up(mtu / mps)
	 * credits = 49 * 3
	 * credits = 147
	 */
	.credits = 147,
	.read_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
};

static const struct l2cap_data le_client_connect_rx_timestamping_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.read_data = l2_data,
	.data_len = sizeof(l2_data),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_RX_SOFTWARE),
};

static const struct l2cap_data le_client_connect_rx_timestamping_32k_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mtu = 672,
	.mps = 251,
	.credits = 147,
	.read_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_RX_SOFTWARE),
};

static const struct l2cap_data le_client_connect_write_success_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.write_data = l2_data,
	.data_len = sizeof(l2_data),
};

static const struct l2cap_data le_client_connect_write_32k_success_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mtu = 672,
	.mps = 251,
	/* Given enough credits to complete the transfer without waiting for
	 * more credits.
	 * credits = round_up(data size / mtu) * round_up(mtu / mps)
	 * credits = 49 * 3
	 * credits = 147
	 */
	.credits = 147,
	.write_data = l2_data_32k,
	.data_len = sizeof(l2_data_32k),
};

static const struct l2cap_data le_client_connect_tx_timestamping_test = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.write_data = l2_data,
	.data_len = sizeof(l2_data),
	.so_timestamping = (SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_OPT_ID |
					SOF_TIMESTAMPING_TX_SOFTWARE |
					SOF_TIMESTAMPING_TX_COMPLETION),
};

static const struct l2cap_data le_client_connect_adv_success_test_1 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.direct_advertising = true,
};

static const struct l2cap_data le_client_connect_success_test_2 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.sec_level  = BT_SECURITY_MEDIUM,
};

static const uint8_t cmd_reject_rsp[] = { 0x01, 0x01, 0x02, 0x00, 0x00, 0x00 };

static const struct l2cap_data le_client_connect_reject_test_1 = {
	.client_psm = 0x0080,
	.send_cmd = cmd_reject_rsp,
	.send_cmd_len = sizeof(cmd_reject_rsp),
	.expect_err = ECONNREFUSED,
};

static const struct l2cap_data le_client_connect_reject_test_2 = {
	.client_psm = 0x0080,
	.addr_type_avail = true,
	.addr_type = BDADDR_LE_PUBLIC,
};

static uint8_t nonexisting_bdaddr[] = {0x00, 0xAA, 0x01, 0x02, 0x03, 0x00};
static const struct l2cap_data le_client_close_socket_test_1 = {
	.client_psm = 0x0080,
	.client_bdaddr = nonexisting_bdaddr,
};

static const struct l2cap_data le_client_close_socket_test_2 = {
	.client_psm = 0x0080,
	.server_not_advertising = true,
};

static const struct l2cap_data le_client_2_same_client = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.server_not_advertising = true,
};

static const struct l2cap_data le_client_2_close_1 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.server_not_advertising = true,
	.close_1 = true,
};

static const struct l2cap_data le_client_connect_nval_psm_test = {
	.client_psm = 0x0080,
	.expect_err = ECONNREFUSED,
};

static const uint8_t le_connect_req[] = {	0x80, 0x00, /* PSM */
						0x41, 0x00, /* SCID */
						0x20, 0x00, /* MTU */
						0x20, 0x00, /* MPS */
						0x05, 0x00, /* Credits */
};

static const uint8_t le_connect_rsp[] = {	0x40, 0x00, /* DCID */
						0xa0, 0x02, /* MTU */
						0xbc, 0x00, /* MPS */
						0x04, 0x00, /* Credits */
						0x00, 0x00, /* Result */
};

static const struct l2cap_data le_server_success_test = {
	.server_psm = 0x0080,
	.send_cmd_code = BT_L2CAP_PDU_LE_CONN_REQ,
	.send_cmd = le_connect_req,
	.send_cmd_len = sizeof(le_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_LE_CONN_RSP,
	.expect_cmd = le_connect_rsp,
	.expect_cmd_len = sizeof(le_connect_rsp),
};

static const uint8_t nval_le_connect_req[] = {	0x80, 0x00, /* PSM */
						0x01, 0x00, /* SCID */
						0x20, 0x00, /* MTU */
						0x20, 0x00, /* MPS */
						0x05, 0x00, /* Credits */
};

static const uint8_t nval_le_connect_rsp[] = {	0x00, 0x00, /* DCID */
						0x00, 0x00, /* MTU */
						0x00, 0x00, /* MPS */
						0x00, 0x00, /* Credits */
						0x09, 0x00, /* Result */
};

static const struct l2cap_data le_server_nval_scid_test = {
	.server_psm = 0x0080,
	.send_cmd_code = BT_L2CAP_PDU_LE_CONN_REQ,
	.send_cmd = nval_le_connect_req,
	.send_cmd_len = sizeof(nval_le_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_LE_CONN_RSP,
	.expect_cmd = nval_le_connect_rsp,
	.expect_cmd_len = sizeof(nval_le_connect_rsp),
};

static const uint8_t ecred_connect_req[] = {	0x80, 0x00, /* PSM */
						0x40, 0x00, /* MTU */
						0x40, 0x00, /* MPS */
						0x05, 0x00, /* Credits */
						0x41, 0x00, /* SCID #1 */
						0x42, 0x00, /* SCID #2 */
						0x43, 0x00, /* SCID #3 */
						0x44, 0x00, /* SCID #4 */
						0x45, 0x00, /* SCID #5 */
};

static const uint8_t ecred_connect_rsp[] = {	0xa0, 0x02, /* MTU */
						0xbc, 0x00, /* MPS */
						0x04, 0x00, /* Credits */
						0x00, 0x00, /* Result */
						0x40, 0x00, /* DCID #1 */
						0x41, 0x00, /* DCID #2 */
						0x42, 0x00, /* DCID #3 */
						0x43, 0x00, /* DCID #4 */
						0x44, 0x00, /* DCID #5 */
};

static const struct l2cap_data ext_flowctl_server_success_test = {
	.server_psm = 0x0080,
	.send_cmd_code = BT_L2CAP_PDU_ECRED_CONN_REQ,
	.send_cmd = ecred_connect_req,
	.send_cmd_len = sizeof(ecred_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_ECRED_CONN_RSP,
	.expect_cmd = ecred_connect_rsp,
	.expect_cmd_len = sizeof(ecred_connect_rsp),
};

static const uint8_t nval_ecred_connect_req[] = {
						0x80, 0x00, /* PSM */
						0x40, 0x00, /* MTU */
						0x40, 0x00, /* MPS */
						0x05, 0x00, /* Credits */
						0x01, 0x00, /* SCID #1 */
};

static const uint8_t nval_ecred_connect_rsp[] = {
						0x00, 0x00, /* MTU */
						0x00, 0x00, /* MPS */
						0x00, 0x00, /* Credits */
						0x09, 0x00, /* Result */
						0x00, 0x00, /* DCID #1 */
};

static const struct l2cap_data ext_flowctl_server_nval_scid_test = {
	.server_psm = 0x0080,
	.send_cmd_code = BT_L2CAP_PDU_ECRED_CONN_REQ,
	.send_cmd = nval_ecred_connect_req,
	.send_cmd_len = sizeof(nval_ecred_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_ECRED_CONN_RSP,
	.expect_cmd = nval_ecred_connect_rsp,
	.expect_cmd_len = sizeof(nval_ecred_connect_rsp),
};

static const struct l2cap_data le_att_client_connect_success_test_1 = {
	.cid = 0x0004,
	.sec_level = BT_SECURITY_LOW,
};

static const struct l2cap_data le_att_server_success_test_1 = {
	.cid = 0x0004,
};

static const struct l2cap_data le_eatt_client_connect_success_test_1 = {
	.client_psm = 0x0027,
	.server_psm = 0x0027,
	.mode = BT_MODE_EXT_FLOWCTL,
	.sec_level = BT_SECURITY_LOW,
};

static const uint8_t eatt_connect_req[] = {	0x27, 0x00, /* PSM */
						0x40, 0x00, /* MTU */
						0x40, 0x00, /* MPS */
						0x05, 0x00, /* Credits */
						0x41, 0x00, /* SCID #1 */
};

static const uint8_t eatt_connect_rsp[] = {	0xa0, 0x02, /* MTU */
						0xbc, 0x00, /* MPS */
						0x04, 0x00, /* Credits */
						0x00, 0x00, /* Result */
						0x40, 0x00, /* DCID #1 */
};

static const struct l2cap_data le_eatt_server_success_test_1 = {
	.server_psm = 0x0027,
	.mode = BT_MODE_EXT_FLOWCTL,
	.send_cmd_code = BT_L2CAP_PDU_ECRED_CONN_REQ,
	.send_cmd = eatt_connect_req,
	.send_cmd_len = sizeof(eatt_connect_req),
	.expect_cmd_code = BT_L2CAP_PDU_ECRED_CONN_RSP,
	.expect_cmd = eatt_connect_rsp,
	.expect_cmd_len = sizeof(eatt_connect_rsp),
	.defer = true,
};

static const uint8_t eatt_reject_req[] = {	0x27, 0x00, /* PSM */
						0x40, 0x00, /* MTU */
						0x40, 0x00, /* MPS */
						0x05, 0x00, /* Credits */
						0x41, 0x00, /* SCID #1 */
						0x42, 0x00, /* SCID #2 */
						0x43, 0x00, /* SCID #3 */
						0x44, 0x00, /* SCID #4 */
						0x45, 0x00, /* SCID #5 */
};

static const uint8_t eatt_reject_rsp[] = {	0xa0, 0x02, /* MTU */
						0xbc, 0x00, /* MPS */
						0x04, 0x00, /* Credits */
						0x06, 0x00, /* Result */
};

static const struct l2cap_data le_eatt_server_reject_test_1 = {
	.server_psm = 0x0027,
	.mode = BT_MODE_EXT_FLOWCTL,
	.send_cmd_code = BT_L2CAP_PDU_ECRED_CONN_REQ,
	.send_cmd = eatt_reject_req,
	.send_cmd_len = sizeof(eatt_reject_req),
	.expect_cmd_code = BT_L2CAP_PDU_ECRED_CONN_RSP,
	.expect_cmd = eatt_reject_rsp,
	.expect_cmd_len = sizeof(eatt_reject_rsp),
	.defer = true,
	.expect_err = -1,
};

static const struct l2cap_data ext_flowctl_client_connect_success_test_1 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
};

static const struct l2cap_data ext_flowctl_client_connect_close_test_1 = {
	.client_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
};

static const struct l2cap_data ext_flowctl_client_connect_timeout_test_1 = {
	.client_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
	.timeout = 1,
};

static const struct l2cap_data ext_flowctl_client_connect_adv_success_test_1 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
	.direct_advertising = true,
};

static const struct l2cap_data ext_flowctl_client_connect_success_test_2 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
	.sec_level  = BT_SECURITY_MEDIUM,
};

static const struct l2cap_data ext_flowctl_client_connect_reject_test_1 = {
	.client_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
	.send_cmd = cmd_reject_rsp,
	.send_cmd_len = sizeof(cmd_reject_rsp),
	.expect_err = ECONNREFUSED,
};

static const struct l2cap_data ext_flowctl_client_2 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
	.server_not_advertising = true,
};

static const struct l2cap_data ext_flowctl_client_2_close_1 = {
	.client_psm = 0x0080,
	.server_psm = 0x0080,
	.mode = BT_MODE_EXT_FLOWCTL,
	.server_not_advertising = true,
	.close_1 = true,
};

static void client_cmd_complete(uint16_t opcode, uint8_t status,
					const void *param, uint8_t len,
					void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *test = data->test_data;
	struct bthost *bthost;

	bthost = hciemu_client_get_host(data->hciemu);

	switch (opcode) {
	case BT_HCI_CMD_WRITE_SCAN_ENABLE:
	case BT_HCI_CMD_LE_SET_ADV_ENABLE:
		tester_print("Client set connectable status 0x%02x", status);
		if (!status && test && test->enable_ssp) {
			bthost_write_ssp_mode(bthost, 0x01);
			return;
		}
		break;
	case BT_HCI_CMD_WRITE_SIMPLE_PAIRING_MODE:
		tester_print("Client enable SSP status 0x%02x", status);
		break;
	default:
		return;
	}


	if (status)
		tester_setup_failed();
	else
		tester_setup_complete();
}

static void server_cmd_complete(uint16_t opcode, uint8_t status,
					const void *param, uint8_t len,
					void *user_data)
{
	switch (opcode) {
	case BT_HCI_CMD_WRITE_SIMPLE_PAIRING_MODE:
		tester_print("Server enable SSP status 0x%02x", status);
		break;
	default:
		return;
	}

	if (status)
		tester_setup_failed();
	else
		tester_setup_complete();
}

static void setup_powered_client_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	struct bthost *bthost;

	if (status != MGMT_STATUS_SUCCESS) {
		tester_setup_failed();
		return;
	}

	tester_print("Controller powered on");

	if (l2data && l2data->timeout) {
		tester_setup_complete();
		return;
	}

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_set_cmd_complete_cb(bthost, client_cmd_complete, user_data);

	if (data->hciemu_type == HCIEMU_TYPE_LE) {
		if (!l2data || !l2data->server_not_advertising)
			bthost_set_adv_enable(bthost, 0x01);
		else
			tester_setup_complete();
	} else {
		bthost_write_scan_enable(bthost, 0x03);
	}
}

static void setup_powered_server_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *test = data->test_data;
	struct bthost *bthost;

	if (status != MGMT_STATUS_SUCCESS) {
		tester_setup_failed();
		return;
	}

	tester_print("Controller powered on");

	if (!test || !test->enable_ssp) {
		tester_setup_complete();
		return;
	}

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_set_cmd_complete_cb(bthost, server_cmd_complete, user_data);
	bthost_write_ssp_mode(bthost, 0x01);
}

static void user_confirm_request_callback(uint16_t index, uint16_t length,
							const void *param,
							void *user_data)
{
	const struct mgmt_ev_user_confirm_request *ev = param;
	struct test_data *data = tester_get_data();
	const struct l2cap_data *test = data->test_data;
	struct mgmt_cp_user_confirm_reply cp;
	uint16_t opcode;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.addr, &ev->addr, sizeof(cp.addr));

	if (test->reject_ssp)
		opcode = MGMT_OP_USER_CONFIRM_NEG_REPLY;
	else
		opcode = MGMT_OP_USER_CONFIRM_REPLY;

	mgmt_reply(data->mgmt, opcode, data->mgmt_index, sizeof(cp), &cp,
							NULL, NULL, NULL);
}

static void pin_code_request_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_ev_pin_code_request *ev = param;
	struct test_data *data = user_data;
	const struct l2cap_data *test = data->test_data;
	struct mgmt_cp_pin_code_reply cp;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.addr, &ev->addr, sizeof(cp.addr));

	if (!test->pin) {
		mgmt_reply(data->mgmt, MGMT_OP_PIN_CODE_NEG_REPLY,
				data->mgmt_index, sizeof(cp.addr), &cp.addr,
				NULL, NULL, NULL);
		return;
	}

	cp.pin_len = test->pin_len;
	memcpy(cp.pin_code, test->pin, test->pin_len);

	mgmt_reply(data->mgmt, MGMT_OP_PIN_CODE_REPLY, data->mgmt_index,
			sizeof(cp), &cp, NULL, NULL, NULL);
}

static void bthost_send_rsp(const void *buf, uint16_t len, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	struct bthost *bthost;

	if (l2data->expect_cmd_len && len != l2data->expect_cmd_len) {
		tester_test_failed();
		return;
	}

	if (l2data->expect_cmd && memcmp(buf, l2data->expect_cmd,
						l2data->expect_cmd_len)) {
		tester_test_failed();
		return;
	}

	if (!l2data->send_cmd)
		return;

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_send_cid(bthost, data->handle, data->dcid,
				l2data->send_cmd, l2data->send_cmd_len);
}

static void send_rsp_new_conn(uint16_t handle, void *user_data)
{
	struct test_data *data = user_data;
	struct bthost *bthost;

	tester_print("New connection with handle 0x%04x", handle);

	data->handle = handle;

	if (data->hciemu_type == HCIEMU_TYPE_LE)
		data->dcid = 0x0005;
	else
		data->dcid = 0x0001;

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_add_cid_hook(bthost, data->handle, data->dcid,
						bthost_send_rsp, NULL);
}

static void setup_powered_common(void)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *test = data->test_data;
	struct bthost *bthost = hciemu_client_get_host(data->hciemu);
	unsigned char param[] = { 0x01 };

	mgmt_register(data->mgmt, MGMT_EV_USER_CONFIRM_REQUEST,
			data->mgmt_index, user_confirm_request_callback,
			NULL, NULL);

	if (test && (test->pin || test->expect_pin))
		mgmt_register(data->mgmt, MGMT_EV_PIN_CODE_REQUEST,
				data->mgmt_index, pin_code_request_callback,
				data, NULL);

	if (test && test->client_io_cap)
		bthost_set_io_capability(bthost, test->client_io_cap);

	if (test && test->client_pin)
		bthost_set_pin_code(bthost, test->client_pin,
							test->client_pin_len);
	if (test && test->reject_ssp)
		bthost_set_reject_user_confirm(bthost, true);

	if (data->hciemu_type == HCIEMU_TYPE_LE)
		mgmt_send(data->mgmt, MGMT_OP_SET_LE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);

	if (test && test->enable_ssp)
		mgmt_send(data->mgmt, MGMT_OP_SET_SSP, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);

	mgmt_send(data->mgmt, MGMT_OP_SET_BONDABLE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);
}

static void setup_powered_client(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *test = data->test_data;
	unsigned char param[] = { 0x01 };

	setup_powered_common();

	tester_print("Powering on controller");

	if (test && (test->expect_cmd || test->send_cmd)) {
		struct bthost *bthost = hciemu_client_get_host(data->hciemu);
		bthost_set_connect_cb(bthost, send_rsp_new_conn, data);
	}

	if (test && test->direct_advertising)
		mgmt_send(data->mgmt, MGMT_OP_SET_ADVERTISING,
				data->mgmt_index, sizeof(param), param,
				NULL, NULL, NULL);

	mgmt_send(data->mgmt, MGMT_OP_SET_POWERED, data->mgmt_index,
			sizeof(param), param, setup_powered_client_callback,
			NULL, NULL);
}

static void setup_powered_server(const void *test_data)
{
	struct test_data *data = tester_get_data();
	unsigned char param[] = { 0x01 };

	setup_powered_common();

	tester_print("Powering on controller");

	mgmt_send(data->mgmt, MGMT_OP_SET_CONNECTABLE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);

	if (data->hciemu_type != HCIEMU_TYPE_BREDR)
		mgmt_send(data->mgmt, MGMT_OP_SET_ADVERTISING,
				data->mgmt_index, sizeof(param), param, NULL,
				NULL, NULL);

	mgmt_send(data->mgmt, MGMT_OP_SET_POWERED, data->mgmt_index,
			sizeof(param), param, setup_powered_server_callback,
			NULL, NULL);
}

static void test_basic(const void *test_data)
{
	int sk;

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		tester_warn("Can't create socket: %s (%d)", strerror(errno),
									errno);
		tester_test_failed();
		return;
	}

	close(sk);

	tester_test_passed();
}

static void received_data(struct test_data *tdata, const void *buf,
					uint16_t len, const void *data,
					uint16_t data_len)
{
	static struct iovec iov;

	util_iov_append(&iov, buf, len);

	tester_debug("read: %d/%zu", len, iov.iov_len);

	/* Check if all the data has been received */
	if (iov.iov_len < data_len)
		return;

	--tdata->step;

	if (iov.iov_len != data_len || memcmp(iov.iov_base, data, data_len))
		tester_test_failed();
	else if (!tdata->step)
		tester_test_passed();

	free(iov.iov_base);
	iov.iov_base = NULL;
	iov.iov_len = 0;
}

static gboolean sock_received_data(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	bool tstamp = l2data->so_timestamping & SOF_TIMESTAMPING_RX_SOFTWARE;
	char buf[1024];
	int sk;
	ssize_t len;

	sk = g_io_channel_unix_get_fd(io);

	len = recv_tstamp(sk, buf, sizeof(buf), tstamp);
	if (len < 0) {
		tester_warn("Unable to read: %s (%d)", strerror(errno), errno);
		tester_test_failed();
		return FALSE;
	}

	received_data(data, buf, len, l2data->read_data, l2data->data_len);

	if (data->step)
		return TRUE;

	return FALSE;
}

static void bthost_received_data(const void *buf, uint16_t len,
							void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;

	received_data(data, buf, len, l2data->write_data, l2data->data_len);
}

static gboolean socket_closed_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	int err, sk_err, sk;
	socklen_t len = sizeof(sk_err);

	tester_print("Disconnected");

	if (l2data->shut_sock_wr) {
		/* if socket is closed using SHUT_WR, L2CAP disconnection
		 * response must be received first before G_IO_HUP event.
		 */
		if (data->host_disconnected)
			tester_test_passed();
		else {
			tester_warn("G_IO_HUP received before receiving L2CAP disconnection");
			tester_test_failed();
		}
	}

	data->io_id = 0;

	sk = g_io_channel_unix_get_fd(io);

	if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &sk_err, &len) < 0)
		err = -errno;
	else
		err = -sk_err;

	if (!l2data->timeout && -err != l2data->expect_err) {
		tester_print("err %d != %d expected_err", -err,
						l2data->expect_err);
		tester_test_failed();
	} else
		tester_test_passed();

	return FALSE;
}

static bool check_mtu(struct test_data *data, int sk)
{
	const struct l2cap_data *l2data = data->test_data;
	socklen_t len;

	memset(&data->l2o, 0, sizeof(data->l2o));

	if (data->hciemu_type == HCIEMU_TYPE_LE &&
				(l2data->client_psm || l2data->server_psm)) {
		/* LE CoC enabled kernels should support BT_RCVMTU and
		 * BT_SNDMTU.
		 */
		len = sizeof(data->l2o.imtu);
		if (getsockopt(sk, SOL_BLUETOOTH, BT_RCVMTU,
						&data->l2o.imtu, &len) < 0) {
			tester_warn("getsockopt(BT_RCVMTU): %s (%d)",
					strerror(errno), errno);
			return false;
		}

		len = sizeof(data->l2o.omtu);
		if (getsockopt(sk, SOL_BLUETOOTH, BT_SNDMTU,
						&data->l2o.omtu, &len) < 0) {
			tester_warn("getsockopt(BT_SNDMTU): %s (%d)",
					strerror(errno), errno);
			return false;
		}

		/* Take SDU len into account */
		data->l2o.imtu -= 2;
		data->l2o.omtu -= 2;
	} else {
		/* For non-LE CoC enabled kernels we need to fall back to
		 * L2CAP_OPTIONS, so test support for it as well */
		len = sizeof(data->l2o);
		if (getsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &data->l2o,
						&len) < 0) {
			 tester_warn("getsockopt(L2CAP_OPTIONS): %s (%d)",
						strerror(errno), errno);
			 return false;
		}
	}

	return true;
}

static gboolean recv_errqueue(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = user_data;
	const struct l2cap_data *l2data = data->test_data;
	int sk = g_io_channel_unix_get_fd(io);
	int err;

	data->step--;

	err = tx_tstamp_recv(&data->tx_ts, sk, l2data->data_len);
	if (err > 0)
		return TRUE;
	else if (err)
		tester_test_failed();
	else if (!data->step)
		tester_test_passed();

	data->err_io_id = 0;
	return FALSE;
}

static void l2cap_tx_timestamping(struct test_data *data, GIOChannel *io)
{
	const struct l2cap_data *l2data = data->test_data;
	int so = l2data->so_timestamping;
	int sk;
	int err;
	unsigned int count;

	if (!(l2data->so_timestamping & TS_TX_RECORD_MASK))
		return;

	sk = g_io_channel_unix_get_fd(io);

	tester_print("Enabling TX timestamping");

	tx_tstamp_init(&data->tx_ts, l2data->so_timestamping,
					l2data->sock_type == SOCK_STREAM);

	for (count = 0; count < l2data->repeat_send + 1; ++count)
		data->step += tx_tstamp_expect(&data->tx_ts, l2data->data_len);

	err = setsockopt(sk, SOL_SOCKET, SO_TIMESTAMPING, &so, sizeof(so));
	if (err < 0) {
		tester_warn("setsockopt SO_TIMESTAMPING: %s (%d)",
						strerror(errno), errno);
		tester_test_failed();
		return;
	}

	data->err_io_id = g_io_add_watch(io, G_IO_ERR, recv_errqueue, data);
}

static int l2cap_send(int sk, const void *data, size_t len, uint16_t mtu)
{
	struct iovec iov = { (void *)data, len };
	int err;
	size_t total = 0;

	len = MIN(mtu, len);

	while (iov.iov_len) {
		size_t l = MIN(iov.iov_len, len);

		err = write(sk, util_iov_pull_mem(&iov, l), l);
		if (err < 0)
			return -errno;

		total += err;
		tester_debug("write: %d/%zu", err, total);
	}

	return total;
}

static void l2cap_read_data(struct test_data *data, GIOChannel *io,
							uint16_t cid)
{
	const struct l2cap_data *l2data = data->test_data;
	struct bthost *bthost;
	struct iovec iov = { (void *)l2data->read_data, l2data->data_len };
	size_t len;

	data->step = 0;

	if (rx_timestamping_init(g_io_channel_unix_get_fd(io),
						l2data->so_timestamping))
		return;

	bthost = hciemu_client_get_host(data->hciemu);
	g_io_add_watch(io, G_IO_IN, sock_received_data, NULL);

	len = MIN(iov.iov_len, data->l2o.imtu);

	while (iov.iov_len) {
		size_t l = MIN(iov.iov_len, len);

		bthost_send_cid(bthost, data->handle, cid,
					util_iov_pull_mem(&iov, l), l);
	}

	++data->step;
}

static void l2cap_write_data(struct test_data *data, GIOChannel *io,
							uint16_t cid)
{
	const struct l2cap_data *l2data = data->test_data;
	struct bthost *bthost;
	ssize_t ret;
	int sk, size;
	unsigned int count;
	socklen_t len = sizeof(size);

	sk = g_io_channel_unix_get_fd(io);

	data->step = 0;

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_add_cid_hook(bthost, data->handle, cid, bthost_received_data,
							NULL);

	l2cap_tx_timestamping(data, io);

	/* Socket buffer needs to hold what we send, btdev doesn't flush now */
	ret = getsockopt(sk, SOL_SOCKET, SO_SNDBUF, &size, &len);
	if (!ret) {
		size += l2data->data_len * (l2data->repeat_send + 1);
		ret = setsockopt(sk, SOL_SOCKET, SO_SNDBUF, &size, len);
		if (ret)
			tester_warn("Failed to set SO_SNDBUF = %d", size);
	}

	for (count = 0; count < l2data->repeat_send + 1; ++count) {
		ret = l2cap_send(sk, l2data->write_data, l2data->data_len,
							data->l2o.omtu);
		if (ret != l2data->data_len) {
			tester_warn("Unable to write all data: "
					"%zd != %u", ret, l2data->data_len);
			tester_test_failed();
		}
		++data->step;
	}
}

static gboolean l2cap_connect_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	int err, sk_err, sk;
	socklen_t len = sizeof(sk_err);

	data->io_id = 0;

	sk = g_io_channel_unix_get_fd(io);

	if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &sk_err, &len) < 0)
		err = -errno;
	else
		err = -sk_err;

	if (err < 0) {
		tester_warn("Connect failed: %s (%d)", strerror(-err), -err);
		goto failed;
	}

	tester_print("Successfully connected to CID 0x%04x", data->dcid);

	if (!check_mtu(data, sk)) {
		tester_test_failed();
		return FALSE;
	}

	if (l2data->read_data) {
		l2cap_read_data(data, io, data->dcid);
		return FALSE;
	} else if (l2data->write_data) {
		l2cap_write_data(data, io, data->dcid);
		return FALSE;
	} else if (l2data->shut_sock_wr) {
		g_io_add_watch(io, G_IO_HUP, socket_closed_cb, NULL);
		shutdown(sk, SHUT_WR);

		return FALSE;
	}

failed:
	if (-err != l2data->expect_err)
		tester_test_failed();
	else
		tester_test_passed();

	return FALSE;
}

static int create_l2cap_sock(struct test_data *data, uint16_t psm,
				uint16_t cid, int sec_level, uint8_t mode)
{
	const struct l2cap_data *l2data = data->test_data;
	const uint8_t *central_bdaddr;
	struct sockaddr_l2 addr;
	int sk, err;
	int sock_type = SOCK_SEQPACKET;

	if (l2data && l2data->sock_type)
		sock_type = l2data->sock_type;

	sk = socket(PF_BLUETOOTH, sock_type | SOCK_NONBLOCK, BTPROTO_L2CAP);
	if (sk < 0) {
		err = -errno;
		tester_warn("Can't create socket: %s (%d)", strerror(errno),
									errno);
		return err;
	}

	central_bdaddr = hciemu_get_central_bdaddr(data->hciemu);
	if (!central_bdaddr) {
		tester_warn("No central bdaddr");
		close(sk);
		return -ENODEV;
	}

	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_psm = htobs(psm);
	addr.l2_cid = htobs(cid);
	bacpy(&addr.l2_bdaddr, (void *) central_bdaddr);

	if (l2data && l2data->addr_type_avail)
		addr.l2_bdaddr_type = l2data->addr_type;
	else if (data->hciemu_type == HCIEMU_TYPE_LE)
		addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
	else
		addr.l2_bdaddr_type = BDADDR_BREDR;

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = -errno;
		tester_warn("Can't bind socket: %s (%d)", strerror(errno),
									errno);
		close(sk);
		return err;
	}

	if (sec_level) {
		struct bt_security sec;

		memset(&sec, 0, sizeof(sec));
		sec.level = sec_level;

		if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &sec,
							sizeof(sec)) < 0) {
			err = -errno;
			tester_warn("Can't set security level: %s (%d)",
						strerror(errno), errno);
			close(sk);
			return err;
		}
	}

	if (mode) {
		if (setsockopt(sk, SOL_BLUETOOTH, BT_MODE, &mode,
							sizeof(mode)) < 0) {
			err = -errno;
			tester_warn("Can't set mode: %s (%d)", strerror(errno),
									errno);
			close(sk);
			return err;
		}
	}

	return sk;
}

static int connect_l2cap_impl(int sk, const uint8_t *bdaddr,
				uint8_t bdaddr_type, uint16_t psm, uint16_t cid)
{
	struct sockaddr_l2 addr;
	int err;

	if (!bdaddr) {
		tester_warn("No client bdaddr");
		return -ENODEV;
	}

	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, (void *) bdaddr);
	addr.l2_bdaddr_type = bdaddr_type;
	addr.l2_psm = htobs(psm);
	addr.l2_cid = htobs(cid);

	err = connect(sk, (struct sockaddr *) &addr, sizeof(addr));
	if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
		err = -errno;
		tester_warn("Can't connect socket: %s (%d)", strerror(errno),
									errno);
		return err;
	}

	return 0;
}

static int connect_l2cap_sock(struct test_data *data, int sk, uint16_t psm,
								uint16_t cid)
{
	const struct l2cap_data *l2data = data->test_data;
	const uint8_t *client_bdaddr;
	uint8_t bdaddr_type;

	if (l2data->client_bdaddr != NULL)
		client_bdaddr = l2data->client_bdaddr;
	else
		client_bdaddr = hciemu_get_client_bdaddr(data->hciemu);

	if (!client_bdaddr) {
		tester_warn("No client bdaddr");
		return -ENODEV;
	}

	if (l2data && l2data->addr_type_avail)
		bdaddr_type = l2data->addr_type;
	else if (data->hciemu_type == HCIEMU_TYPE_LE)
		bdaddr_type = BDADDR_LE_PUBLIC;
	else
		bdaddr_type = BDADDR_BREDR;

	return connect_l2cap_impl(sk, client_bdaddr, bdaddr_type, psm, cid);
}

static void client_l2cap_connect_cb(uint16_t handle, uint16_t cid,
							void *user_data)
{
	struct test_data *data = user_data;

	tester_debug("Client connect CID 0x%04x handle 0x%04x", cid, handle);

	data->dcid = cid;
	data->handle = handle;
}

static void client_l2cap_disconnect_cb(void *user_data)
{
	struct test_data *data = user_data;

	data->host_disconnected = true;
}

static void direct_adv_cmd_complete(uint16_t opcode, const void *param,
						uint8_t len, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct bt_hci_cmd_le_set_adv_parameters *cp;
	const uint8_t *expect_bdaddr;

	if (opcode != BT_HCI_CMD_LE_SET_ADV_PARAMETERS)
		return;

	tester_print("Received advertising parameters HCI command");

	cp = param;

	/* Advertising as client should be direct advertising */
	if (cp->type != 0x01) {
		tester_warn("Invalid advertising type");
		tester_test_failed();
		return;
	}

	expect_bdaddr = hciemu_get_client_bdaddr(data->hciemu);
	if (memcmp(expect_bdaddr, cp->direct_addr, 6)) {
		tester_warn("Invalid direct address in adv params");
		tester_test_failed();
		return;
	}

	tester_test_passed();
}

static void test_connect(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	GIOChannel *io;
	int sk;

	if (l2data->server_psm) {
		struct bthost *bthost = hciemu_client_get_host(data->hciemu);
		bthost_l2cap_connect_cb host_connect_cb = NULL;
		bthost_l2cap_disconnect_cb host_disconnect_cb = NULL;

		if (l2data->data_len)
			host_connect_cb = client_l2cap_connect_cb;

		if (l2data->shut_sock_wr)
			host_disconnect_cb = client_l2cap_disconnect_cb;

		if (l2data->mtu || l2data->mps || l2data->credits)
			bthost_add_l2cap_server_custom(bthost,
							l2data->server_psm,
							l2data->mtu,
							l2data->mps,
							l2data->credits,
							host_connect_cb,
							host_disconnect_cb,
							data);
		else
			bthost_add_l2cap_server(bthost, l2data->server_psm,
							host_connect_cb,
							host_disconnect_cb,
							data);
	}

	if (l2data->direct_advertising)
		hciemu_add_central_post_command_hook(data->hciemu,
						direct_adv_cmd_complete, NULL);

	sk = create_l2cap_sock(data, 0, l2data->cid, l2data->sec_level,
							l2data->mode);
	if (sk < 0) {
		if (sk == -ENOPROTOOPT)
			tester_test_abort();
		else
			tester_test_failed();
		return;
	}

	if (connect_l2cap_sock(data, sk, l2data->client_psm,
							l2data->cid) < 0) {
		close(sk);
		tester_test_failed();
		return;
	}

	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);

	data->io_id = g_io_add_watch(io, G_IO_OUT, l2cap_connect_cb, NULL);

	g_io_channel_unref(io);

	tester_print("Connect in progress");
}

static void test_connect_close(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	GIOChannel *io;
	int sk;

	sk = create_l2cap_sock(data, 0, l2data->cid, l2data->sec_level,
							l2data->mode);
	if (sk < 0) {
		if (sk == -ENOPROTOOPT)
			tester_test_abort();
		else
			tester_test_failed();
		return;
	}

	if (connect_l2cap_sock(data, sk, l2data->client_psm,
							l2data->cid) < 0) {
		close(sk);
		tester_test_failed();
		return;
	}

	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);
	data->io_id = g_io_add_watch(io, G_IO_HUP, socket_closed_cb, NULL);
	g_io_channel_unref(io);

	shutdown(sk, SHUT_RDWR);
}

static void test_connect_timeout(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	GIOChannel *io;
	int sk;
	struct timeval sndto;
	socklen_t len;

	sk = create_l2cap_sock(data, 0, l2data->cid, l2data->sec_level,
							l2data->mode);
	if (sk < 0) {
		if (sk == -ENOPROTOOPT)
			tester_test_abort();
		else
			tester_test_failed();
		return;
	}

	memset(&sndto, 0, sizeof(sndto));

	sndto.tv_sec = l2data->timeout;
	len = sizeof(sndto);
	if (setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &sndto, len) < 0) {
		tester_print("Can't set SO_SNDTIMEO: %s (%d)", strerror(errno),
								errno);
		close(sk);
		tester_test_failed();
		return;
	}

	if (connect_l2cap_sock(data, sk, l2data->client_psm,
							l2data->cid) < 0) {
		close(sk);
		tester_test_failed();
		return;
	}

	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);
	data->io_id = g_io_add_watch(io, G_IO_HUP, socket_closed_cb, NULL);
	g_io_channel_unref(io);
}

static void test_connect_reject(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	int sk;

	sk = create_l2cap_sock(data, 0, l2data->cid, l2data->sec_level,
							l2data->mode);
	if (sk < 0) {
		tester_test_failed();
		return;
	}

	if (connect_l2cap_sock(data, sk, l2data->client_psm,
							l2data->cid) < 0)
		tester_test_passed();
	else
		tester_test_failed();

	close(sk);
}

static int connect_socket(const uint8_t *client_bdaddr, GIOFunc connect_cb,
								bool defer)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	GIOChannel *io;
	int sk;

	sk = create_l2cap_sock(data, 0, l2data->cid, l2data->sec_level,
							l2data->mode);
	if (sk < 0) {
		tester_print("Error in create_l2cap_sock");
		if (sk == -ENOPROTOOPT)
			tester_test_abort();
		else
			tester_test_failed();
		return -1;
	}

	if (defer) {
		int opt = 1;

		if (setsockopt(sk, SOL_BLUETOOTH, BT_DEFER_SETUP, &opt,
							sizeof(opt)) < 0) {
			tester_print("Can't enable deferred setup: %s (%d)",
						strerror(errno), errno);
			tester_test_failed();
			return -1;
		}
	}

	if (connect_l2cap_impl(sk, client_bdaddr, BDADDR_LE_PUBLIC,
			l2data->client_psm, l2data->cid) < 0) {
		tester_print("Error in connect_l2cap_sock");
		close(sk);
		tester_test_failed();
		return -1;
	}

	if (connect_cb) {
		io = g_io_channel_unix_new(sk);
		g_io_channel_set_close_on_unref(io, TRUE);

		data->io_id = g_io_add_watch(io, G_IO_OUT, connect_cb, NULL);

		g_io_channel_unref(io);
	}

	tester_print("Connect in progress, sk = %d %s", sk,
				defer ? "(deferred)" : "");

	return sk;
}

static gboolean test_close_socket_1_part_3(gpointer arg)
{
	struct test_data *data = tester_get_data();

	tester_print("Checking whether scan was properly stopped...");

	if (data->sk != -1) {
		tester_print("Error - scan was not enabled yet");
		tester_test_failed();
		return FALSE;
	}

	if (hciemu_get_central_le_scan_enable(data->hciemu)) {
		tester_print("Delayed check whether scann is off failed");
		tester_test_failed();
		return FALSE;
	}

	tester_test_passed();
	return FALSE;
}

static gboolean test_close_socket_1_part_2(gpointer args)
{
	struct test_data *data = tester_get_data();
	int sk = data->sk;

	tester_print("Will close socket during scan phase...");

	/* We tried to connect to LE device that is not advertising. It
	 * was added to kernel accept list, and scan was started. We
	 * should be still scanning.
	 */
	if (!hciemu_get_central_le_scan_enable(data->hciemu)) {
		tester_print("Error - should be still scanning");
		tester_test_failed();
		return FALSE;
	}

	/* Calling close() should remove device from  accept list, and stop
	 * the scan.
	 */
	if (close(sk) < 0) {
		tester_print("Error when closing socket");
		tester_test_failed();
		return FALSE;
	}

	data->sk = -1;
	/* tester_test_passed will be called when scan is stopped. */
	return FALSE;
}

static gboolean test_close_socket_2_part_3(gpointer arg)
{
	struct test_data *data = tester_get_data();
	int sk = data->sk;
	int err;

	/* Scan should be already over, we're trying to create connection */
	if (hciemu_get_central_le_scan_enable(data->hciemu)) {
		tester_print("Error - should no longer scan");
		tester_test_failed();
		return FALSE;
	}

	/* Calling close() should eventually cause CMD_LE_CREATE_CONN_CANCEL */
	err = close(sk);
	if (err < 0) {
		tester_print("Error when closing socket");
		tester_test_failed();
		return FALSE;
	}

	/* CMD_LE_CREATE_CONN_CANCEL will trigger test pass. */
	return FALSE;
}

static bool test_close_socket_cc_hook(const void *data, uint16_t len,
							void *user_data)
{
	return false;
}

static gboolean test_close_socket_2_part_2(gpointer arg)
{
	struct test_data *data = tester_get_data();
	struct bthost *bthost = hciemu_client_get_host(data->hciemu);

	/* Make sure CMD_LE_CREATE_CONN will not immediately result in
	 * BT_HCI_EVT_CONN_COMPLETE.
	 */
	hciemu_add_hook(data->hciemu, HCIEMU_HOOK_PRE_EVT,
		BT_HCI_CMD_LE_CREATE_CONN, test_close_socket_cc_hook, NULL);

	/* Advertise once. After that, kernel should stop scanning, and trigger
	 * BT_HCI_CMD_LE_CREATE_CONN_CANCEL.
	 */
	bthost_set_adv_enable(bthost, 0x01);
	bthost_set_adv_enable(bthost, 0x00);
	return FALSE;
}

static void test_close_socket_scan_enabled(void)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;

	if (l2data == &le_client_close_socket_test_1)
		g_idle_add(test_close_socket_1_part_2, NULL);
	else if (l2data == &le_client_close_socket_test_2)
		g_idle_add(test_close_socket_2_part_2, NULL);
}

static void test_close_socket_scan_disabled(void)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;

	if (l2data == &le_client_close_socket_test_1)
		g_idle_add(test_close_socket_1_part_3, NULL);
	else if (l2data == &le_client_close_socket_test_2)
		g_idle_add(test_close_socket_2_part_3, NULL);
}

static void test_close_socket_conn_cancel(void)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;

	if (l2data == &le_client_close_socket_test_2)
		tester_test_passed();
}

static void test_close_socket_router(uint16_t opcode, const void *param,
					uint8_t length, void *user_data)
{
	/* tester_print("HCI Command 0x%04x length %u", opcode, length); */
	if (opcode == BT_HCI_CMD_LE_SET_SCAN_ENABLE) {
		const struct bt_hci_cmd_le_set_scan_enable *scan_params = param;

		if (scan_params->enable == true)
			test_close_socket_scan_enabled();
		else
			test_close_socket_scan_disabled();
	} else if (opcode == BT_HCI_CMD_LE_CREATE_CONN_CANCEL) {
		test_close_socket_conn_cancel();
	}
}

static void test_close_socket(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	const uint8_t *client_bdaddr;

	hciemu_add_central_post_command_hook(data->hciemu,
					test_close_socket_router, data);

	if (l2data->client_bdaddr != NULL)
		client_bdaddr = l2data->client_bdaddr;
	else
		client_bdaddr = hciemu_get_client_bdaddr(data->hciemu);

	data->sk = connect_socket(client_bdaddr, NULL, false);
}

static uint8_t test_2_connect_cb_cnt;
static gboolean test_2_connect_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	int err, sk_err, sk;
	socklen_t len = sizeof(sk_err);

	data->io_id = 0;

	sk = g_io_channel_unix_get_fd(io);

	if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &sk_err, &len) < 0)
		err = -errno;
	else
		err = -sk_err;

	if (err < 0) {
		tester_warn("Connect failed: %s (%d)", strerror(-err), -err);
		tester_test_failed();
		return FALSE;
	}

	tester_print("Successfully connected");
	test_2_connect_cb_cnt++;

	if (test_2_connect_cb_cnt == 2) {
		close(data->sk);
		close(data->sk2);
		tester_test_passed();
	}

	if (l2data->close_1 && test_2_connect_cb_cnt == 1) {
		close(data->sk2);
		tester_test_passed();
	}

	return FALSE;
}

static gboolean enable_advertising(gpointer args)
{
	struct test_data *data = tester_get_data();
	struct bthost *bthost = hciemu_client_get_host(data->hciemu);

	bthost_set_adv_enable(bthost, 0x01);
	return FALSE;
}

static void test_connect_2_part_2(void)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	const uint8_t *client_bdaddr;

	client_bdaddr = hciemu_get_client_bdaddr(data->hciemu);
	data->sk2 = connect_socket(client_bdaddr, test_2_connect_cb, false);

	if (l2data->close_1) {
		tester_print("Closing first socket! %d", data->sk);
		close(data->sk);
	}

	g_idle_add(enable_advertising, NULL);
}

static uint8_t test_scan_enable_counter;
static void test_connect_2_router(uint16_t opcode, const void *param,
					uint8_t length, void *user_data)
{
	const struct bt_hci_cmd_le_set_scan_enable *scan_params = param;

	tester_print("HCI Command 0x%04x length %u", opcode, length);
	if (opcode == BT_HCI_CMD_LE_SET_SCAN_ENABLE &&
						scan_params->enable == true) {
		test_scan_enable_counter++;
		if (test_scan_enable_counter == 1)
			test_connect_2_part_2();
		else if (test_scan_enable_counter == 2)
			g_idle_add(enable_advertising, NULL);
	}
}

static void test_connect_2(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	const uint8_t *client_bdaddr;
	bool defer;

	test_2_connect_cb_cnt = 0;
	test_scan_enable_counter = 0;

	hciemu_add_central_post_command_hook(data->hciemu,
				test_connect_2_router, data);

	if (l2data->server_psm) {
		struct bthost *bthost = hciemu_client_get_host(data->hciemu);

		if (!l2data->data_len)
			bthost_add_l2cap_server(bthost, l2data->server_psm,
						NULL, NULL, NULL);
	}

	defer = (l2data->mode == BT_MODE_EXT_FLOWCTL);

	client_bdaddr = hciemu_get_client_bdaddr(data->hciemu);
	if (l2data->close_1)
		data->sk = connect_socket(client_bdaddr, NULL, defer);
	else
		data->sk = connect_socket(client_bdaddr, test_2_connect_cb,
								defer);
}

static gboolean l2cap_accept_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	int sk;

	sk = g_io_channel_unix_get_fd(io);

	if (!check_mtu(data, sk)) {
		tester_test_failed();
		return FALSE;
	}

	if (l2data->read_data) {
		l2cap_read_data(data, io, data->dcid);
		return FALSE;
	} else if (l2data->write_data) {
		l2cap_write_data(data, io, data->scid);
		return FALSE;
	}

	tester_print("Successfully connected");

	tester_test_passed();

	return FALSE;
}

static bool defer_accept(struct test_data *data, GIOChannel *io)
{
	int sk;
	char c;
	struct pollfd pfd;

	sk = g_io_channel_unix_get_fd(io);

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = sk;
	pfd.events = POLLOUT;

	if (poll(&pfd, 1, 0) < 0) {
		tester_warn("poll: %s (%d)", strerror(errno), errno);
		return false;
	}

	if (!(pfd.revents & POLLOUT)) {
		if (read(sk, &c, 1) < 0) {
			tester_warn("read: %s (%d)", strerror(errno), errno);
			return false;
		}
	}

	data->io_id = g_io_add_watch(io, G_IO_OUT, l2cap_accept_cb, NULL);

	g_io_channel_unref(io);

	tester_print("Accept deferred setup");

	return true;
}

static gboolean l2cap_listen_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	int sk, new_sk;

	data->io_id = 0;

	sk = g_io_channel_unix_get_fd(io);

	new_sk = accept(sk, NULL, NULL);
	if (new_sk < 0) {
		tester_warn("accept failed: %s (%u)", strerror(errno), errno);
		tester_test_failed();
		return FALSE;
	}

	io = g_io_channel_unix_new(new_sk);
	g_io_channel_set_close_on_unref(io, TRUE);

	if (l2data->defer) {
		if (l2data->expect_err < 0) {
			g_io_channel_unref(io);
			return FALSE;
		}

		if (!defer_accept(data, io)) {
			tester_warn("Unable to accept deferred setup");
			tester_test_failed();
		}
		return FALSE;
	}

	return l2cap_accept_cb(io, cond, user_data);
}

static void client_l2cap_rsp(uint8_t code, const void *data, uint16_t len,
							void *user_data)
{
	struct test_data *test_data = user_data;
	const struct l2cap_data *l2data = test_data->test_data;

	tester_print("Client received response code 0x%02x", code);

	if (code != l2data->expect_cmd_code) {
		tester_warn("Unexpected L2CAP response code (expected 0x%02x)",
						l2data->expect_cmd_code);
		goto failed;
	}

	if (code == BT_L2CAP_PDU_CONN_RSP) {

		const struct bt_l2cap_pdu_conn_rsp *rsp = data;
		if (len == sizeof(rsp) && !rsp->result && !rsp->status)
			return;

		test_data->dcid = rsp->dcid;
		test_data->scid = rsp->scid;

		if (l2data->data_len)
			return;
	}

	if (!l2data->expect_cmd) {
		tester_test_passed();
		return;
	}

	if (l2data->expect_cmd_len != len) {
		tester_warn("Unexpected L2CAP response length (%u != %u)",
						len, l2data->expect_cmd_len);
		goto failed;
	}

	if (memcmp(l2data->expect_cmd, data, len) != 0) {
		tester_warn("Unexpected L2CAP response");
		goto failed;
	}

	tester_test_passed();
	return;

failed:
	tester_test_failed();
}

static void send_req_new_conn(uint16_t handle, void *user_data)
{
	struct test_data *data = user_data;
	const struct l2cap_data *l2data = data->test_data;
	struct bthost *bthost;

	tester_print("New client connection with handle 0x%04x", handle);

	data->handle = handle;

	if (l2data->send_cmd) {
		bthost_l2cap_rsp_cb cb;

		if (l2data->expect_cmd_code)
			cb = client_l2cap_rsp;
		else
			cb = NULL;

		tester_print("Sending L2CAP Request from client");

		bthost = hciemu_client_get_host(data->hciemu);
		bthost_l2cap_req(bthost, handle, l2data->send_cmd_code,
					l2data->send_cmd, l2data->send_cmd_len,
					cb, data);
	}
}

static void test_server(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const struct l2cap_data *l2data = data->test_data;
	const uint8_t *central_bdaddr;
	uint8_t addr_type;
	struct bthost *bthost;
	GIOChannel *io;
	int sk;

	if (l2data->server_psm || l2data->cid) {
		int opt = 1;

		sk = create_l2cap_sock(data, l2data->server_psm,
					l2data->cid, l2data->sec_level,
					l2data->mode);
		if (sk < 0) {
			tester_test_failed();
			return;
		}

		if (l2data->defer && setsockopt(sk, SOL_BLUETOOTH,
				BT_DEFER_SETUP, &opt, sizeof(opt)) < 0) {
			tester_warn("Can't enable deferred setup: %s (%d)",
						strerror(errno), errno);
			tester_test_failed();
			close(sk);
			return;
		}

		if (listen(sk, 5) < 0) {
			tester_warn("listening on socket failed: %s (%u)",
					strerror(errno), errno);
			tester_test_failed();
			close(sk);
			return;
		}

		io = g_io_channel_unix_new(sk);
		g_io_channel_set_close_on_unref(io, TRUE);

		data->io_id = g_io_add_watch(io, G_IO_IN, l2cap_listen_cb,
									NULL);
		g_io_channel_unref(io);

		tester_print("Listening for connections");
	}

	central_bdaddr = hciemu_get_central_bdaddr(data->hciemu);
	if (!central_bdaddr) {
		tester_warn("No central bdaddr");
		tester_test_failed();
		return;
	}

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_set_connect_cb(bthost, send_req_new_conn, data);

	if (data->hciemu_type == HCIEMU_TYPE_BREDR)
		addr_type = BDADDR_BREDR;
	else
		addr_type = BDADDR_LE_PUBLIC;

	bthost_hci_connect(bthost, central_bdaddr, addr_type);
}

static void test_getpeername_not_connected(const void *test_data)
{
	struct test_data *data = tester_get_data();
	struct sockaddr_l2 addr;
	socklen_t len;
	int sk;

	sk = create_l2cap_sock(data, 0, 0, 0, 0);
	if (sk < 0) {
		tester_test_failed();
		return;
	}

	len = sizeof(addr);
	if (getpeername(sk, (struct sockaddr *) &addr, &len) == 0) {
		tester_warn("getpeername succeeded on non-connected socket");
		tester_test_failed();
		goto done;
	}

	if (errno != ENOTCONN) {
		tester_warn("Unexpected getpeername error: %s (%d)",
						strerror(errno), errno);
		tester_test_failed();
		goto done;
	}

	tester_test_passed();

done:
	close(sk);
}

static void test_l2cap_ethtool_get_ts_info(const void *test_data)
{
	struct test_data *data = tester_get_data();

	test_ethtool_get_ts_info(data->mgmt_index, BTPROTO_L2CAP, false);
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	test_l2cap_bredr("Basic L2CAP Socket - Success", NULL,
					setup_powered_client, test_basic);
	test_l2cap_bredr("Non-connected getpeername - Failure", NULL,
					setup_powered_client,
					test_getpeername_not_connected);

	test_l2cap_bredr("L2CAP BR/EDR Client - Success",
					&client_connect_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Close",
					&client_connect_close_test,
					setup_powered_client,
					test_connect_close);
	test_l2cap_bredr("L2CAP BR/EDR Client - Timeout",
					&client_connect_timeout_test,
					setup_powered_client,
					test_connect_timeout);

	test_l2cap_bredr("L2CAP BR/EDR Client SSP - Success 1",
					&client_connect_ssp_success_test_1,
					setup_powered_client, test_connect);
	test_l2cap_bredr("L2CAP BR/EDR Client SSP - Success 2",
					&client_connect_ssp_success_test_2,
					setup_powered_client, test_connect);
	test_l2cap_bredr("L2CAP BR/EDR Client PIN Code - Success",
					&client_connect_pin_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Read Success",
					&client_connect_read_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Read 32k Success",
					&client_connect_read_32k_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - RX Timestamping",
					&client_connect_rx_timestamping_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - RX Timestamping 32k",
				&client_connect_rx_timestamping_32k_test,
				setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Write Success",
					&client_connect_write_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Write 32k Success",
					&client_connect_write_32k_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - TX Timestamping",
					&client_connect_tx_timestamping_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Stream TX Timestamping",
				&client_connect_stream_tx_timestamping_test,
				setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Invalid PSM 1",
					&client_connect_nval_psm_test_1,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Invalid PSM 2",
					&client_connect_nval_psm_test_2,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Invalid PSM 3",
					&client_connect_nval_psm_test_3,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Client - Socket Shut WR Success",
					&client_connect_shut_wr_success_test,
					setup_powered_client, test_connect);

	test_l2cap_bredr("L2CAP BR/EDR Server - Success",
					&l2cap_server_success_test,
					setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Server - Read Success",
					&l2cap_server_read_success_test,
					setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Server - Read 32k Success",
					&l2cap_server_read_32k_success_test,
					setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Server - Write Success",
					&l2cap_server_write_success_test,
					setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Server - Write 32k Success",
					&l2cap_server_write_32k_success_test,
					setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Server - Security Block",
					&l2cap_server_sec_block_test,
					setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Server - Invalid PSM",
					&l2cap_server_nval_psm_test,
					setup_powered_server, test_server);
	test_l2cap_bredr("L2CAP BR/EDR Server - Invalid PDU",
				&l2cap_server_nval_pdu_test1,
				setup_powered_server, test_server);
	test_l2cap_bredr("L2CAP BR/EDR Server - Invalid Disconnect CID",
				&l2cap_server_nval_cid_test1,
				setup_powered_server, test_server);
	test_l2cap_bredr("L2CAP BR/EDR Server - Invalid Config CID",
				&l2cap_server_nval_cid_test2,
				setup_powered_server, test_server);

	test_l2cap_bredr("L2CAP BR/EDR Ethtool Get Ts Info - Success", NULL,
			setup_powered_server, test_l2cap_ethtool_get_ts_info);

	test_l2cap_le("L2CAP LE Client - Success",
				&le_client_connect_success_test_1,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - Close",
				&le_client_connect_close_test_1,
				setup_powered_client, test_connect_close);
	test_l2cap_le("L2CAP LE Client - Timeout",
				&le_client_connect_timeout_test_1,
				setup_powered_client, test_connect_timeout);
	test_l2cap_le("L2CAP LE Client - Read Success",
				&le_client_connect_read_success_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - Read 32k Success",
				&le_client_connect_read_32k_success_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - RX Timestamping",
				&le_client_connect_rx_timestamping_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - RX Timestamping 32k",
				&le_client_connect_rx_timestamping_32k_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - Write Success",
				&le_client_connect_write_success_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - Write 32k Success",
				&le_client_connect_write_32k_success_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - TX Timestamping",
				&le_client_connect_tx_timestamping_test,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client, Direct Advertising - Success",
				&le_client_connect_adv_success_test_1,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client SMP - Success",
				&le_client_connect_success_test_2,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Client - Command Reject",
					&le_client_connect_reject_test_1,
					setup_powered_client, test_connect);
	test_l2cap_bredr("L2CAP LE Client - Connection Reject",
				&le_client_connect_reject_test_2,
				setup_powered_client, test_connect_reject);

	test_l2cap_le("L2CAP LE Client - Close socket 1",
				&le_client_close_socket_test_1,
				setup_powered_client,
				test_close_socket);

	test_l2cap_le("L2CAP LE Client - Close socket 2",
				&le_client_close_socket_test_2,
				setup_powered_client,
				test_close_socket);

	test_l2cap_le("L2CAP LE Client - Open two sockets",
				&le_client_2_same_client,
				setup_powered_client,
				test_connect_2);

	test_l2cap_le("L2CAP LE Client - Open two sockets close one",
				&le_client_2_close_1,
				setup_powered_client,
				test_connect_2);

	test_l2cap_le("L2CAP LE Client - Invalid PSM",
					&le_client_connect_nval_psm_test,
					setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE Server - Success", &le_server_success_test,
					setup_powered_server, test_server);
	test_l2cap_le("L2CAP LE Server - Nval SCID", &le_server_nval_scid_test,
					setup_powered_server, test_server);


	test_l2cap_le("L2CAP Ext-Flowctl Client - Success",
				&ext_flowctl_client_connect_success_test_1,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP Ext-Flowctl Client - Close",
				&ext_flowctl_client_connect_close_test_1,
				setup_powered_client, test_connect_close);
	test_l2cap_le("L2CAP Ext-Flowctl Client - Timeout",
				&ext_flowctl_client_connect_timeout_test_1,
				setup_powered_client, test_connect_timeout);
	test_l2cap_le("L2CAP Ext-Flowctl Client, Direct Advertising - Success",
				&ext_flowctl_client_connect_adv_success_test_1,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP Ext-Flowctl Client SMP - Success",
				&ext_flowctl_client_connect_success_test_2,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP Ext-Flowctl Client - Command Reject",
				&ext_flowctl_client_connect_reject_test_1,
				setup_powered_client, test_connect);

	test_l2cap_le("L2CAP Ext-Flowctl Client - Open two sockets",
				&ext_flowctl_client_2,
				setup_powered_client,
				test_connect_2);

	test_l2cap_le("L2CAP Ext-Flowctl Client - Open two sockets close one",
				&ext_flowctl_client_2_close_1,
				setup_powered_client,
				test_connect_2);

	test_l2cap_le("L2CAP Ext-Flowctl Server - Success",
				&ext_flowctl_server_success_test,
				setup_powered_server, test_server);
	test_l2cap_le("L2CAP Ext-Flowctl Server - Nval SCID",
				&ext_flowctl_server_nval_scid_test,
				setup_powered_server, test_server);

	test_l2cap_le("L2CAP LE ATT Client - Success",
				&le_att_client_connect_success_test_1,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE ATT Server - Success",
				&le_att_server_success_test_1,
				setup_powered_server, test_server);

	test_l2cap_le("L2CAP LE EATT Client - Success",
				&le_eatt_client_connect_success_test_1,
				setup_powered_client, test_connect);
	test_l2cap_le("L2CAP LE EATT Server - Success",
				&le_eatt_server_success_test_1,
				setup_powered_server, test_server);
	test_l2cap_le("L2CAP LE EATT Server - Reject",
				&le_eatt_server_reject_test_1,
				setup_powered_server, test_server);

	test_l2cap_le("L2CAP LE Ethtool Get Ts Info - Success", NULL,
			setup_powered_server, test_l2cap_ethtool_get_ts_info);

	return tester_run();
}
