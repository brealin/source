/*
 * hostapd / ubus support
 * Copyright (c) 2013, Felix Fietkau <nbd@nbd.name>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "utils/wpabuf.h"
#include "common/ieee802_11_defs.h"
#include "hostapd.h"
#include "neighbor_db.h"
#include "wps_hostapd.h"
#include "sta_info.h"
#include "ubus.h"
#include "ap_drv_ops.h"
#include "beacon.h"

static struct ubus_context *ctx;
static struct blob_buf b;
static int ctx_ref;

static inline struct hostapd_data *get_hapd_from_object(struct ubus_object *obj)
{
	return container_of(obj, struct hostapd_data, ubus.obj);
}


struct ubus_banned_client {
	struct avl_node avl;
	u8 addr[ETH_ALEN];
};

static void ubus_receive(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct ubus_context *ctx = eloop_ctx;
	ubus_handle_event(ctx);
}

static void ubus_reconnect_timeout(void *eloop_data, void *user_ctx)
{
	if (ubus_reconnect(ctx, NULL)) {
		eloop_register_timeout(1, 0, ubus_reconnect_timeout, ctx, NULL);
		return;
	}

	eloop_register_read_sock(ctx->sock.fd, ubus_receive, ctx, NULL);
}

static void hostapd_ubus_connection_lost(struct ubus_context *ctx)
{
	eloop_unregister_read_sock(ctx->sock.fd);
	eloop_register_timeout(1, 0, ubus_reconnect_timeout, ctx, NULL);
}

static bool hostapd_ubus_init(void)
{
	if (ctx)
		return true;

	ctx = ubus_connect(NULL);
	if (!ctx)
		return false;

	ctx->connection_lost = hostapd_ubus_connection_lost;
	eloop_register_read_sock(ctx->sock.fd, ubus_receive, ctx, NULL);
	return true;
}

static void hostapd_ubus_ref_inc(void)
{
	ctx_ref++;
}

static void hostapd_ubus_ref_dec(void)
{
	ctx_ref--;
	if (!ctx)
		return;

	if (ctx_ref)
		return;

	eloop_unregister_read_sock(ctx->sock.fd);
	ubus_free(ctx);
	ctx = NULL;
}

void hostapd_ubus_add_iface(struct hostapd_iface *iface)
{
	if (!hostapd_ubus_init())
		return;
}

void hostapd_ubus_free_iface(struct hostapd_iface *iface)
{
	if (!ctx)
		return;
}

static void
hostapd_bss_del_ban(void *eloop_data, void *user_ctx)
{
	struct ubus_banned_client *ban = eloop_data;
	struct hostapd_data *hapd = user_ctx;

	avl_delete(&hapd->ubus.banned, &ban->avl);
	free(ban);
}

static void
hostapd_bss_ban_client(struct hostapd_data *hapd, u8 *addr, int time)
{
	struct ubus_banned_client *ban;

	if (time < 0)
		time = 0;

	ban = avl_find_element(&hapd->ubus.banned, addr, ban, avl);
	if (!ban) {
		if (!time)
			return;

		ban = os_zalloc(sizeof(*ban));
		memcpy(ban->addr, addr, sizeof(ban->addr));
		ban->avl.key = ban->addr;
		avl_insert(&hapd->ubus.banned, &ban->avl);
	} else {
		eloop_cancel_timeout(hostapd_bss_del_ban, ban, hapd);
		if (!time) {
			hostapd_bss_del_ban(ban, hapd);
			return;
		}
	}

	eloop_register_timeout(0, time * 1000, hostapd_bss_del_ban, ban, hapd);
}

static int
hostapd_bss_get_clients(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct hostapd_data *hapd = container_of(obj, struct hostapd_data, ubus.obj);
	struct sta_info *sta;
	void *list, *c;
	char mac_buf[20];
	static const struct {
		const char *name;
		uint32_t flag;
	} sta_flags[] = {
		{ "auth", WLAN_STA_AUTH },
		{ "assoc", WLAN_STA_ASSOC },
		{ "authorized", WLAN_STA_AUTHORIZED },
		{ "preauth", WLAN_STA_PREAUTH },
		{ "wds", WLAN_STA_WDS },
		{ "wmm", WLAN_STA_WMM },
		{ "ht", WLAN_STA_HT },
		{ "vht", WLAN_STA_VHT },
		{ "wps", WLAN_STA_WPS },
		{ "mfp", WLAN_STA_MFP },
	};

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "freq", hapd->iface->freq);
	list = blobmsg_open_table(&b, "clients");
	for (sta = hapd->sta_list; sta; sta = sta->next) {
		int i;

		sprintf(mac_buf, MACSTR, MAC2STR(sta->addr));
		c = blobmsg_open_table(&b, mac_buf);
		for (i = 0; i < ARRAY_SIZE(sta_flags); i++)
			blobmsg_add_u8(&b, sta_flags[i].name,
				       !!(sta->flags & sta_flags[i].flag));
		blobmsg_add_u32(&b, "aid", sta->aid);
		blobmsg_close_table(&b, c);
	}
	blobmsg_close_array(&b, list);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	NOTIFY_RESPONSE,
	__NOTIFY_MAX
};

static const struct blobmsg_policy notify_policy[__NOTIFY_MAX] = {
	[NOTIFY_RESPONSE] = { "notify_response", BLOBMSG_TYPE_INT32 },
};

static int
hostapd_notify_response(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__NOTIFY_MAX];
	struct hostapd_data *hapd = get_hapd_from_object(obj);
	struct wpabuf *elems;
	const char *pos;
	size_t len;

	blobmsg_parse(notify_policy, __NOTIFY_MAX, tb,
		      blob_data(msg), blob_len(msg));

	if (!tb[NOTIFY_RESPONSE])
		return UBUS_STATUS_INVALID_ARGUMENT;

	hapd->ubus.notify_response = blobmsg_get_u32(tb[NOTIFY_RESPONSE]);

	return UBUS_STATUS_OK;
}

enum {
	DEL_CLIENT_ADDR,
	DEL_CLIENT_REASON,
	DEL_CLIENT_DEAUTH,
	DEL_CLIENT_BAN_TIME,
	__DEL_CLIENT_MAX
};

static const struct blobmsg_policy del_policy[__DEL_CLIENT_MAX] = {
	[DEL_CLIENT_ADDR] = { "addr", BLOBMSG_TYPE_STRING },
	[DEL_CLIENT_REASON] = { "reason", BLOBMSG_TYPE_INT32 },
	[DEL_CLIENT_DEAUTH] = { "deauth", BLOBMSG_TYPE_INT8 },
	[DEL_CLIENT_BAN_TIME] = { "ban_time", BLOBMSG_TYPE_INT32 },
};

static int
hostapd_bss_del_client(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__DEL_CLIENT_MAX];
	struct hostapd_data *hapd = container_of(obj, struct hostapd_data, ubus.obj);
	struct sta_info *sta;
	bool deauth = false;
	int reason;
	u8 addr[ETH_ALEN];

	blobmsg_parse(del_policy, __DEL_CLIENT_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[DEL_CLIENT_ADDR])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (hwaddr_aton(blobmsg_data(tb[DEL_CLIENT_ADDR]), addr))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (tb[DEL_CLIENT_REASON])
		reason = blobmsg_get_u32(tb[DEL_CLIENT_REASON]);

	if (tb[DEL_CLIENT_DEAUTH])
		deauth = blobmsg_get_bool(tb[DEL_CLIENT_DEAUTH]);

	sta = ap_get_sta(hapd, addr);
	if (sta) {
		if (deauth) {
			hostapd_drv_sta_deauth(hapd, addr, reason);
			ap_sta_deauthenticate(hapd, sta, reason);
		} else {
			hostapd_drv_sta_disassoc(hapd, addr, reason);
			ap_sta_disassociate(hapd, sta, reason);
		}
	}

	if (tb[DEL_CLIENT_BAN_TIME])
		hostapd_bss_ban_client(hapd, addr, blobmsg_get_u32(tb[DEL_CLIENT_BAN_TIME]));

	return 0;
}

static void
blobmsg_add_macaddr(struct blob_buf *buf, const char *name, const u8 *addr)
{
	char *s;

	s = blobmsg_alloc_string_buffer(buf, name, 20);
	sprintf(s, MACSTR, MAC2STR(addr));
	blobmsg_add_string_buffer(buf);
}

static int
hostapd_bss_list_bans(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct hostapd_data *hapd = container_of(obj, struct hostapd_data, ubus.obj);
	struct ubus_banned_client *ban;
	void *c;

	blob_buf_init(&b, 0);
	c = blobmsg_open_array(&b, "clients");
	avl_for_each_element(&hapd->ubus.banned, ban, avl)
		blobmsg_add_macaddr(&b, NULL, ban->addr);
	blobmsg_close_array(&b, c);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int
hostapd_bss_wps_start(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	int rc;
	struct hostapd_data *hapd = container_of(obj, struct hostapd_data, ubus.obj);

	rc = hostapd_wps_button_pushed(hapd, NULL);

	if (rc != 0)
		return UBUS_STATUS_NOT_SUPPORTED;

	return 0;
}

static int
hostapd_bss_wps_cancel(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	int rc;
	struct hostapd_data *hapd = container_of(obj, struct hostapd_data, ubus.obj);

	rc = hostapd_wps_cancel(hapd);

	if (rc != 0)
		return UBUS_STATUS_NOT_SUPPORTED;

	return 0;
}

static int
hostapd_bss_update_beacon(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	int rc;
	struct hostapd_data *hapd = container_of(obj, struct hostapd_data, ubus.obj);

	rc = ieee802_11_set_beacon(hapd);

	if (rc != 0)
		return UBUS_STATUS_NOT_SUPPORTED;

	return 0;
}

enum {
	CSA_FREQ,
	CSA_BCN_COUNT,
	__CSA_MAX
};

static const struct blobmsg_policy csa_policy[__CSA_MAX] = {
	/*
	 * for now, frequency and beacon count are enough, add more
	 * parameters on demand
	 */
	[CSA_FREQ] = { "freq", BLOBMSG_TYPE_INT32 },
	[CSA_BCN_COUNT] = { "bcn_count", BLOBMSG_TYPE_INT32 },
};

#ifdef NEED_AP_MLME
static int
hostapd_switch_chan(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__CSA_MAX];
	struct hostapd_data *hapd = get_hapd_from_object(obj);
	struct csa_settings css;

	blobmsg_parse(csa_policy, __CSA_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[CSA_FREQ])
		return UBUS_STATUS_INVALID_ARGUMENT;

	memset(&css, 0, sizeof(css));
	css.freq_params.freq = blobmsg_get_u32(tb[CSA_FREQ]);
	if (tb[CSA_BCN_COUNT])
		css.cs_count = blobmsg_get_u32(tb[CSA_BCN_COUNT]);

	if (hostapd_switch_channel(hapd, &css) != 0)
		return UBUS_STATUS_NOT_SUPPORTED;
	return UBUS_STATUS_OK;
}
#endif

enum {
	VENDOR_ELEMENTS,
	__VENDOR_ELEMENTS_MAX
};

static const struct blobmsg_policy ve_policy[__VENDOR_ELEMENTS_MAX] = {
	/* vendor elements are provided as hex-string */
	[VENDOR_ELEMENTS] = { "vendor_elements", BLOBMSG_TYPE_STRING },
};

static int
hostapd_vendor_elements(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__VENDOR_ELEMENTS_MAX];
	struct hostapd_data *hapd = get_hapd_from_object(obj);
	struct hostapd_bss_config *bss = hapd->conf;
	struct wpabuf *elems;
	const char *pos;
	size_t len;

	blobmsg_parse(ve_policy, __VENDOR_ELEMENTS_MAX, tb,
		      blob_data(msg), blob_len(msg));

	if (!tb[VENDOR_ELEMENTS])
		return UBUS_STATUS_INVALID_ARGUMENT;

	pos = blobmsg_data(tb[VENDOR_ELEMENTS]);
	len = os_strlen(pos);
	if (len & 0x01)
			return UBUS_STATUS_INVALID_ARGUMENT;

	len /= 2;
	if (len == 0) {
		wpabuf_free(bss->vendor_elements);
		bss->vendor_elements = NULL;
		return 0;
	}

	elems = wpabuf_alloc(len);
	if (elems == NULL)
		return 1;

	if (hexstr2bin(pos, wpabuf_put(elems, len), len)) {
		wpabuf_free(elems);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	wpabuf_free(bss->vendor_elements);
	bss->vendor_elements = elems;

	/* update beacons if vendor elements were set successfully */
	if (ieee802_11_update_beacons(hapd->iface) != 0)
		return UBUS_STATUS_NOT_SUPPORTED;
	return UBUS_STATUS_OK;
}

static void
hostapd_rrm_print_nr(struct hostapd_neighbor_entry *nr)
{
	const u8 *data;
	char *str;
	int len;

	blobmsg_printf(&b, "", MACSTR, MAC2STR(nr->bssid));

	str = blobmsg_alloc_string_buffer(&b, "", nr->ssid.ssid_len + 1);
	memcpy(str, nr->ssid.ssid, nr->ssid.ssid_len);
	str[nr->ssid.ssid_len] = 0;
	blobmsg_add_string_buffer(&b);

	len = wpabuf_len(nr->nr);
	str = blobmsg_alloc_string_buffer(&b, "", 2 * len + 1);
	wpa_snprintf_hex(str, 2 * len + 1, wpabuf_head_u8(nr->nr), len);
	blobmsg_add_string_buffer(&b);
}

static int
hostapd_rrm_nr_get_own(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct hostapd_data *hapd = get_hapd_from_object(obj);
	struct hostapd_neighbor_entry *nr;
	void *c;

	if (!(hapd->conf->radio_measurements[0] & WLAN_RRM_CAPS_NEIGHBOR_REPORT))
		return UBUS_STATUS_NOT_SUPPORTED;

	nr = hostapd_neighbor_get(hapd, hapd->own_addr, NULL);
	if (!nr)
		return UBUS_STATUS_NOT_FOUND;

	blob_buf_init(&b, 0);

	c = blobmsg_open_array(&b, "value");
	hostapd_rrm_print_nr(nr);
	blobmsg_close_array(&b, c);

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int
hostapd_rrm_nr_list(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct hostapd_data *hapd = get_hapd_from_object(obj);
	struct hostapd_neighbor_entry *nr;
	void *c;

	if (!(hapd->conf->radio_measurements[0] & WLAN_RRM_CAPS_NEIGHBOR_REPORT))
		return UBUS_STATUS_NOT_SUPPORTED;

	blob_buf_init(&b, 0);

	c = blobmsg_open_array(&b, "list");
	dl_list_for_each(nr, &hapd->nr_db, struct hostapd_neighbor_entry, list) {
		void *cur;

		if (!memcmp(nr->bssid, hapd->own_addr, ETH_ALEN))
			continue;

		cur = blobmsg_open_array(&b, NULL);
		hostapd_rrm_print_nr(nr);
		blobmsg_close_array(&b, cur);
	}
	blobmsg_close_array(&b, c);

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	NR_SET_LIST,
	__NR_SET_LIST_MAX
};

static const struct blobmsg_policy nr_set_policy[__NR_SET_LIST_MAX] = {
	[NR_SET_LIST] = { "list", BLOBMSG_TYPE_ARRAY },
};


static void
hostapd_rrm_nr_clear(struct hostapd_data *hapd)
{
	struct hostapd_neighbor_entry *nr;

restart:
	dl_list_for_each(nr, &hapd->nr_db, struct hostapd_neighbor_entry, list) {
		if (!memcmp(nr->bssid, hapd->own_addr, ETH_ALEN))
			continue;

		hostapd_neighbor_remove(hapd, nr->bssid, &nr->ssid);
		goto restart;
	}
}

static int
hostapd_rrm_nr_set(struct ubus_context *ctx, struct ubus_object *obj,
		   struct ubus_request_data *req, const char *method,
		   struct blob_attr *msg)
{
	static const struct blobmsg_policy nr_e_policy[] = {
		{ .type = BLOBMSG_TYPE_STRING },
		{ .type = BLOBMSG_TYPE_STRING },
		{ .type = BLOBMSG_TYPE_STRING },
	};
	struct hostapd_data *hapd = get_hapd_from_object(obj);
	struct blob_attr *tb_l[__NR_SET_LIST_MAX];
	struct blob_attr *tb[ARRAY_SIZE(nr_e_policy)];
	struct blob_attr *cur;
	int ret = 0;
	int rem;

	if (!(hapd->conf->radio_measurements[0] & WLAN_RRM_CAPS_NEIGHBOR_REPORT))
		return UBUS_STATUS_NOT_SUPPORTED;

	blobmsg_parse(nr_set_policy, __NR_SET_LIST_MAX, tb_l, blob_data(msg), blob_len(msg));
	if (!tb_l[NR_SET_LIST])
		return UBUS_STATUS_INVALID_ARGUMENT;

	hostapd_rrm_nr_clear(hapd);
	blobmsg_for_each_attr(cur, tb_l[NR_SET_LIST], rem) {
		struct wpa_ssid_value ssid;
		struct wpabuf *data;
		u8 bssid[ETH_ALEN];
		char *s;

		blobmsg_parse_array(nr_e_policy, ARRAY_SIZE(nr_e_policy), tb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[0] || !tb[1] || !tb[2])
			goto invalid;

		s = blobmsg_get_string(tb[0]);
		if (hwaddr_aton(s, bssid))
			goto invalid;

		s = blobmsg_get_string(tb[1]);
		ssid.ssid_len = strlen(s);
		if (ssid.ssid_len > sizeof(ssid.ssid))
			goto invalid;

		memcpy(&ssid, s, ssid.ssid_len);
		data = wpabuf_parse_bin(blobmsg_get_string(tb[2]));
		if (!data)
			goto invalid;

		hostapd_neighbor_set(hapd, bssid, &ssid, data, NULL, NULL, 0);
		wpabuf_free(data);
		continue;

invalid:
		ret = UBUS_STATUS_INVALID_ARGUMENT;
	}

	return 0;
}

static const struct ubus_method bss_methods[] = {
	UBUS_METHOD_NOARG("get_clients", hostapd_bss_get_clients),
	UBUS_METHOD("del_client", hostapd_bss_del_client, del_policy),
	UBUS_METHOD_NOARG("list_bans", hostapd_bss_list_bans),
	UBUS_METHOD_NOARG("wps_start", hostapd_bss_wps_start),
	UBUS_METHOD_NOARG("wps_cancel", hostapd_bss_wps_cancel),
	UBUS_METHOD_NOARG("update_beacon", hostapd_bss_update_beacon),
#ifdef NEED_AP_MLME
	UBUS_METHOD("switch_chan", hostapd_switch_chan, csa_policy),
#endif
	UBUS_METHOD("set_vendor_elements", hostapd_vendor_elements, ve_policy),
	UBUS_METHOD("notify_response", hostapd_notify_response, notify_policy),
	UBUS_METHOD_NOARG("rrm_nr_get_own", hostapd_rrm_nr_get_own),
	UBUS_METHOD_NOARG("rrm_nr_list", hostapd_rrm_nr_list),
	UBUS_METHOD("rrm_nr_set", hostapd_rrm_nr_set, nr_set_policy),
};

static struct ubus_object_type bss_object_type =
	UBUS_OBJECT_TYPE("hostapd_bss", bss_methods);

static int avl_compare_macaddr(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, ETH_ALEN);
}

void hostapd_ubus_add_bss(struct hostapd_data *hapd)
{
	struct ubus_object *obj = &hapd->ubus.obj;
	char *name;
	int ret;

	if (!hostapd_ubus_init())
		return;

	if (asprintf(&name, "hostapd.%s", hapd->conf->iface) < 0)
		return;

	avl_init(&hapd->ubus.banned, avl_compare_macaddr, false, NULL);
	obj->name = name;
	obj->type = &bss_object_type;
	obj->methods = bss_object_type.methods;
	obj->n_methods = bss_object_type.n_methods;
	ret = ubus_add_object(ctx, obj);
	hostapd_ubus_ref_inc();
}

void hostapd_ubus_free_bss(struct hostapd_data *hapd)
{
	struct ubus_object *obj = &hapd->ubus.obj;
	char *name = (char *) obj->name;

	if (!ctx)
		return;

	if (obj->id) {
		ubus_remove_object(ctx, obj);
		hostapd_ubus_ref_dec();
	}

	free(name);
}

struct ubus_event_req {
	struct ubus_notify_request nreq;
	bool deny;
};

static void
ubus_event_cb(struct ubus_notify_request *req, int idx, int ret)
{
	struct ubus_event_req *ureq = container_of(req, struct ubus_event_req, nreq);

	if (ret)
		ureq->deny = true;
}

int hostapd_ubus_handle_event(struct hostapd_data *hapd, struct hostapd_ubus_request *req)
{
	struct ubus_banned_client *ban;
	const char *types[HOSTAPD_UBUS_TYPE_MAX] = {
		[HOSTAPD_UBUS_PROBE_REQ] = "probe",
		[HOSTAPD_UBUS_AUTH_REQ] = "auth",
		[HOSTAPD_UBUS_ASSOC_REQ] = "assoc",
	};
	const char *type = "mgmt";
	struct ubus_event_req ureq = {};
	const u8 *addr;

	if (req->mgmt_frame)
		addr = req->mgmt_frame->sa;
	else
		addr = req->addr;

	ban = avl_find_element(&hapd->ubus.banned, addr, ban, avl);
	if (ban)
		return -2;

	if (!hapd->ubus.obj.has_subscribers)
		return 0;

	if (req->type < ARRAY_SIZE(types))
		type = types[req->type];

	blob_buf_init(&b, 0);
	blobmsg_add_macaddr(&b, "address", addr);
	if (req->mgmt_frame)
		blobmsg_add_macaddr(&b, "target", req->mgmt_frame->da);
	if (req->frame_info)
		blobmsg_add_u32(&b, "signal", req->frame_info->ssi_signal);
	blobmsg_add_u32(&b, "freq", hapd->iface->freq);

	if (!hapd->ubus.notify_response) {
		ubus_notify(ctx, &hapd->ubus.obj, type, b.head, -1);
		return 0;
	}

	if (ubus_notify_async(ctx, &hapd->ubus.obj, type, b.head, &ureq.nreq))
		return 0;

	ureq.nreq.status_cb = ubus_event_cb;
	ubus_complete_request(ctx, &ureq.nreq.req, 100);

	if (ureq.deny)
		return -1;

	return 0;
}

void hostapd_ubus_notify(struct hostapd_data *hapd, const char *type, const u8 *addr)
{
	char mac[18];

	if (!hapd->ubus.obj.has_subscribers)
		return;

	if (!addr)
		return;

	snprintf(mac, sizeof(mac), MACSTR, MAC2STR(addr));

	blob_buf_init(&b, 0);
	blobmsg_add_macaddr(&b, "address", mac);

	ubus_notify(ctx, &hapd->ubus.obj, type, b.head, -1);
}
