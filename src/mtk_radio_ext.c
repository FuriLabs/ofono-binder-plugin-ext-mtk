/*
 *  oFono - Open Source Telephony - binder based adaptation MTK plugin
 *
 *  Copyright (C) 2022 Jolla Ltd.
 *  Copyright (C) 2024 TheKit <thekit@disroot.org>
 *  Copyright (C) 2024 Furi Labs
 *  Copyright (C) 2024 Bardia Moshiri <bardia@furilabs.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <glib-object.h>

#include "mtk_radio_ext.h"
#include "mtk_radio_ext_types.h"
#include "binder_util.h"
#include "dbus_ext.h"

#include <ofono/log.h>
#include <ofono/misc.h>

#include <gbinder.h>

#include <gutil_idlepool.h>
#include <gutil_log.h>
#include <gutil_macros.h>

#define MTK_RADIO_CALL_TIMEOUT (3*1000) /* ms */

typedef GObjectClass MtkRadioExtClass;
typedef struct mtk_radio_ext {
    GObject parent;
    char* slot;
    GBinderClient* client;
    GBinderRemoteObject* remote;
    GBinderLocalObject* ims_response;
    GBinderLocalObject* ims_indication;
    GBinderLocalObject* mtk_response;
    GBinderLocalObject* mtk_indication;
    GBinderLocalObject* atci_response;
    GBinderLocalObject* atci_indication;
    GUtilIdlePool* pool;
    GHashTable* requests;
} MtkRadioExt;

GType mtk_radio_ext_get_type() G_GNUC_INTERNAL;
G_DEFINE_TYPE(MtkRadioExt, mtk_radio_ext, G_TYPE_OBJECT)

#define THIS_TYPE mtk_radio_ext_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, MtkRadioExt)
#define PARENT_CLASS mtk_radio_ext_parent_class
#define KEY(serial) GUINT_TO_POINTER(serial)

enum mtk_radio_ext_signal {
    SIGNAL_IMS_REG_STATUS_CHANGED,
    SIGNAL_IMS_REGISTRATION_INFO_CHANGED,
    SIGNAL_CALL_INFO,
    SIGNAL_COUNT
};

#define SIGNAL_IMS_REG_STATUS_CHANGED_NAME        "mtk-radio-ext-ims-reg-status-changed"
#define SIGNAL_IMS_REGISTRATION_INFO_CHANGED_NAME "mtk-radio-ext-ims-registration-info-changed"
#define SIGNAL_CALL_INFO_NAME                     "mtk-radio-ext-call-info"

static guint mtk_radio_ext_signals[SIGNAL_COUNT] = { 0 };

typedef struct mtk_radio_ext_request MtkRadioExtRequest;

typedef void (*MtkRadioExtArgWriteFunc)(
    GBinderWriter* args,
    va_list va);

typedef void (*MtkRadioExtRequestHandlerFunc)(
    MtkRadioExtRequest* req,
    const RadioResponseInfo* info,
    const GBinderReader* args);

typedef void (*MtkRadioExtResultFunc)(
    MtkRadioExt* radio,
    int result,
    void* user_data);

struct mtk_radio_ext_request {
    guint id;  /* request id */
    gulong tx; /* binder transaction id */
    MtkRadioExt* radio;
    gint32 response_code;
    MtkRadioExtRequestHandlerFunc handle_response;
    void (*free)(MtkRadioExtRequest* req);
    GDestroyNotify destroy;
    void* user_data;
};

typedef struct mtk_radio_ext_result_request {
    MtkRadioExtRequest base;
    MtkRadioExtResultFunc complete;
} MtkRadioExtResultRequest;

static GLogModule mtk_radio_ext_binder_log_module = {
    .max_level = GLOG_LEVEL_VERBOSE,
    .level = GLOG_LEVEL_VERBOSE,
    .flags = GLOG_FLAG_HIDE_NAME
};

static GLogModule mtk_radio_ext_binder_dump_module = {
    .parent = &mtk_radio_ext_binder_log_module,
    .max_level = GLOG_LEVEL_VERBOSE,
    .level = GLOG_LEVEL_INHERIT,
    .flags = GLOG_FLAG_HIDE_NAME
};

static
void
mtk_radio_ext_log_notify(
    struct ofono_debug_desc* desc)
{
    mtk_radio_ext_binder_log_module.level = (desc->flags &
        OFONO_DEBUG_FLAG_PRINT) ? GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static
void
mtk_radio_ext_dump_notify(
    struct ofono_debug_desc* desc)
{
    mtk_radio_ext_binder_dump_module.level = (desc->flags &
        OFONO_DEBUG_FLAG_PRINT) ? GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static struct ofono_debug_desc logger_trace OFONO_DEBUG_ATTR = {
    .name = "mtk_binder_trace",
    .flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
    .notify = mtk_radio_ext_log_notify
};

static struct ofono_debug_desc logger_dump OFONO_DEBUG_ATTR = {
    .name = "mtk_binder_dump",
    .flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
    .notify = mtk_radio_ext_dump_notify
};

static
guint
mtk_radio_ext_new_req_id()
{
    static guint last_id = 0;
    // start from 1
    return ++last_id;
}

static const char*
mtk_radio_ext_req_name(
    guint32 req)
{
    switch (req) {
#define MTK_RADIO_REQ_(req, resp, name, NAME) \
        case MTK_RADIO_REQ_##NAME: return #name;
    MTK_RADIO_EXT_IMS_CALL_3_0(MTK_RADIO_REQ_)
#undef MTK_RADIO_REQ_
    }
    return NULL;
}

static const char*
mtk_radio_ext_resp_name(
    guint32 resp)
{
    switch (resp) {
#define IMS_RADIO_RESP_(req, resp, name, NAME) \
        case IMS_RADIO_RESP_##NAME: return #name;
    MTK_RADIO_EXT_IMS_CALL_3_0(IMS_RADIO_RESP_)
#undef IMS_RADIO_RESP_
    }
    return NULL;
}

static const char*
mtk_radio_ext_ind_name(
    guint32 ind)
{
    switch (ind) {
#define IMS_RADIO_IND_(code, name, NAME) \
        case IMS_RADIO_IND_##NAME: return #name;
    IMS_RADIO_INDICATION_3_0(IMS_RADIO_IND_)
#undef IMS_RADIO_IND_
    }
    return NULL;
}

static
void
mtk_radio_ext_log_req(
    MtkRadioExt* self,
    guint32 code,
    guint32 serial)
{
    static const GLogModule* log = &mtk_radio_ext_binder_log_module;
    const int level = GLOG_LEVEL_INHERIT;
    const char* name;

    if (!gutil_log_enabled(log, level))
        return;

    name = mtk_radio_ext_req_name(code);

    if (serial) {
        gutil_log(log, level, "%s< [%08x] %u %s",
            self->slot, serial, code, name ? name : "???");
    } else {
        gutil_log(log, level, "%s< %u %s",
            self->slot, code, name ? name : "???");
    }
}

void
mtk_radio_ext_log_resp(
    MtkRadioExt* self,
    guint32 code,
    guint32 serial)
{
    static const GLogModule* log = &mtk_radio_ext_binder_log_module;
    const int level = GLOG_LEVEL_INHERIT;
    const char* name;

    if (!gutil_log_enabled(log, level))
        return;

    name = mtk_radio_ext_resp_name(code);

    gutil_log(log, level, "%s> [%08x] %u %s",
        self->slot, serial, code, name ? name : "???");
}

static
void
mtk_radio_ext_log_ind(
    MtkRadioExt* self,
    guint32 code)
{
    static const GLogModule* log = &mtk_radio_ext_binder_log_module;
    const int level = GLOG_LEVEL_INHERIT;
    const char* name;

    if (!gutil_log_enabled(log, level))
        return;

    name = mtk_radio_ext_ind_name(code);

    gutil_log(log, level, "%s > %u %s", self->slot, code,
        name ? name : "???");
}

static
void
mtk_radio_ext_dump_data(
    const GBinderReader* reader)
{
    static const GLogModule* log = &mtk_radio_ext_binder_dump_module;
    const int level = GLOG_LEVEL_INHERIT;
    gsize size;
    const guint8* data;

    if (!gutil_log_enabled(log, level))
        return;

    data = gbinder_reader_get_data(reader, &size);
    gutil_log_dump(log, level, "  ",  data, size);
}

static
void
mtk_radio_ext_dump_request(
    GBinderLocalRequest* args)
{
    static const GLogModule* log = &mtk_radio_ext_binder_dump_module;
    const int level = GLOG_LEVEL_INHERIT;
    GBinderWriter writer;
    const guint8* data;
    gsize size;

    if (!gutil_log_enabled(log, level))
        return;

    /* Use writer API to fetch the raw data */
    gbinder_local_request_init_writer(args, &writer);
    data = gbinder_writer_get_data(&writer, &size);
    gutil_log_dump(log, level, "  ", data, size);
}

static
gulong
mtk_radio_ext_call(
    MtkRadioExt* self,
    gint32 code,
    gint32 serial,
    GBinderLocalRequest* req,
    GBinderClientReplyFunc reply,
    GDestroyNotify destroy,
    void* user_data)
{
    mtk_radio_ext_log_req(self, code, serial);
    mtk_radio_ext_dump_request(req);

    return gbinder_client_transact(self->client, code,
        GBINDER_TX_FLAG_ONEWAY, req, reply, destroy, user_data);
}

#define MAYBE_HIDL_STRING(hstr) ((hstr).data.str ? (hstr).data.str : "")

static
const IncomingCallNotification*
mtk_radio_ext_read_incoming_call_notification(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    const IncomingCallNotification* notification;

    gbinder_reader_copy(&reader, args);
    notification = gbinder_reader_read_hidl_struct(&reader, IncomingCallNotification);
    if (notification) {
        DBG("%s: IncomingCallNotification callId:%s number:%s type:%s\n"
            "callMode:%s seqNo:%s redirectNumber:%s toNumber:%s",
            self->slot,
            MAYBE_HIDL_STRING(notification->callId),
            MAYBE_HIDL_STRING(notification->number),
            MAYBE_HIDL_STRING(notification->type),
            MAYBE_HIDL_STRING(notification->callMode),
            MAYBE_HIDL_STRING(notification->seqNo),
            MAYBE_HIDL_STRING(notification->redirectNumber),
            MAYBE_HIDL_STRING(notification->toNumber));
        return notification;
    } else {
        DBG("%s: failed to parse IncomingCallNotification", self->slot);
        return NULL;
    }
}

static
void
mtk_radio_ext_handle_incoming_call_indication(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    /* incomingCallIndication(RadioIndicationType type, IncomingCallNotification inCallNotify) */
    const IncomingCallNotification* notification =
        mtk_radio_ext_read_incoming_call_notification(self, args);

    if (notification) {
        /* Allow incoming call via setCallIndication */
        /* setCallIndication(int32_t serial, int32_t mode, int32_t callId, int32_t seqNumber, int32_t cause) */
        const guint code = MTK_RADIO_REQ_SET_CALL_INDICATION;
        GBinderClient* client = self->client;
        GBinderLocalRequest* req = gbinder_client_new_request2(client, code);
        GBinderWriter writer;
        guint serial = mtk_radio_ext_new_req_id();

        gbinder_local_request_init_writer(req, &writer);
        gbinder_writer_append_int32(&writer, serial); /* serial */
        gbinder_writer_append_int32(&writer, IMS_ALLOW_INCOMING_CALL_INDICATION); /* mode */
        gbinder_writer_append_int32(&writer, atoi(MAYBE_HIDL_STRING(notification->callId))); /* callId */
        gbinder_writer_append_int32(&writer, atoi(MAYBE_HIDL_STRING(notification->seqNo))); /* seqNumber */
        gbinder_writer_append_int32(&writer, -1); /* cause = unknown? */

        mtk_radio_ext_call(self, code, serial, req, NULL, NULL, NULL);
        gbinder_local_request_unref(req);
    }
}

static
void
mtk_radio_ext_handle_call_info_indication(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    /* callInfoIndication(RadioIndicationType type, vec<string> data) */
    GBinderReader reader;
    char** data;

    gbinder_reader_copy(&reader, args);
    data = gbinder_reader_read_hidl_string_vec(&reader);

    if (data && g_strv_length(data) >= 6) {
        /* +ECPI:<call_id>, <msg_type>, <is_ibt>, <is_tch>,
         *       <dir>, <call_mode>, <number>, <toa>, [<cause>] */

        /* other values seem to be hardcoded sender side and irrelevant for us */
        guint call_id = atoi(data[0]);
        guint msg_type = atoi(data[1]);
        guint call_mode = atoi(data[5]);
        char* number = data[6];

        DBG("%s: callInfoIndication callId:%d msgType:%d callMode:%d number:%s",
            self->slot, call_id, msg_type, call_mode, number);

        g_signal_emit(self, mtk_radio_ext_signals[SIGNAL_CALL_INFO],
                      0, call_id, msg_type, call_mode, number);

        g_strfreev(data);
    } else {
        DBG("%s: failed to parse callInfoIndication data", self->slot);
    }
}

static
const ImsRegStatusInfo*
mtk_radio_ext_read_ims_reg_status_info(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    const ImsRegStatusInfo* info;

    gbinder_reader_copy(&reader, args);
    info = gbinder_reader_read_hidl_struct(&reader, ImsRegStatusInfo);
    if (info) {
        DBG("%s: ImsRegStatusInfo report_type:%d account_id:%d"
            " expire_time:%d error_code:%d\n"
            " uri:%s error_msg:%s",
            self->slot,
            info->report_type,
            info->account_id,
            info->expire_time,
            info->error_code,
            MAYBE_HIDL_STRING(info->uri),
            MAYBE_HIDL_STRING(info->error_msg));
        return info;
    } else {
        DBG("%s: failed to parse ImsRegStatusInfo", self->slot);
        return NULL;
    }
}

static
void
mtk_radio_ext_handle_ims_reg_status_report(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    /* imsRegStatusReport(RadioIndicationType type, ImsRegStatusInfo report) */
    const ImsRegStatusInfo* info =
        mtk_radio_ext_read_ims_reg_status_info(self, args);

    g_signal_emit(self, mtk_radio_ext_signals[SIGNAL_IMS_REG_STATUS_CHANGED],
                    0, info->report_type);
}

static
void
mtk_radio_ext_handle_ims_registration_info(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    gbinder_reader_copy(&reader, args);
    int register_state = 0, capability = 0;

    /* imsRegistrationInfo(RadioIndicationType type, int32_t registerState, int32_t capability) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &register_state);
    gbinder_reader_read_int32(&reader, &capability);

    DBG("%s: IMS Registration info (imsRegistrationInfo): register state: %d, capability: %d",
        self->slot, register_state, capability);

    g_signal_emit(self, mtk_radio_ext_signals[SIGNAL_IMS_REGISTRATION_INFO_CHANGED],
                    0, register_state, capability);
}

static
void
mtk_radio_ext_handle_ims_bearer_init(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    /* imsBearerInit(RadioIndicationType type) */
    DBG("%s: IMS Bearer successfully initialized (imsBearerInit)", self->slot);
}

static
void
mtk_radio_ext_handle_speech_codec_info_indication(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    int info = 0;

    /* speechCodecInfoIndication(RadioIndicationType type, int32_t info) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &info);

    DBG("%s: Speech codec info (speechCodecInfoIndication): %d", self->slot, info);
}

static
void
mtk_radio_ext_handle_ims_cfg_feature_changed(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    int phone_id = 0, feature_id = 0, value = 0;

    /* imsCfgFeatureChanged(RadioIndicationType type, int32_t phoneId, int32_t featureId, int32_t value) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &phone_id);
    gbinder_reader_read_int32(&reader, &feature_id);
    gbinder_reader_read_int32(&reader, &value);

    DBG("%s: IMS Feature changed (imsCfgFeatureChanged): phone id: %d, feature id: %d, value: %d",
        self->slot, phone_id, feature_id, value);
}

static
void
mtk_radio_ext_handle_ims_cfg_config_changed(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    int phone_id = 0;

    /* imsCfgConfigChanged(RadioIndicationType type, int32_t phoneId, string configId, string value) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &phone_id);
    const char* config_id = gbinder_reader_read_hidl_string_c(&reader);
    const char* value = gbinder_reader_read_hidl_string_c(&reader);

    DBG("%s: IMS Config changed (imsCfgConfigChanged): phone id: %d, config id: %s, value: %s",
        self->slot, phone_id, config_id, value);
}

static
void
mtk_radio_ext_handle_ims_cfg_config_loaded(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    /* imsCfgConfigLoaded(RadioIndicationType type) */
    DBG("%s: IMS Config loaded (imsCfgConfigLoaded)", self->slot);
}

static
void
mtk_radio_ext_handle_ims_cfg_dynamic_ims_switch_complete(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    /* imsCfgDynamicImsSwitchComplete(RadioIndicationType type) */
    DBG("%s: IMS dynamic configuration switch completed (imsCfgDynamicImsSwitchComplete)", self->slot);
}

static
void
mtk_radio_ext_handle_sip_reg_info_ind(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    int account_id = 0, response_code = 0;
    char** data;

    /* sipRegInfoInd(RadioIndicationType type, int32_t account_id, int32_t response_code, vec<string> info) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &account_id);
    gbinder_reader_read_int32(&reader, &response_code);
    data = gbinder_reader_read_hidl_string_vec(&reader);

    if (data && g_strv_length(data) >= 4) {
        /* 1. direction
         * 2. SIP_msg_type
         * 3. method
         * 4. reason_phrase
         * 5. warn_text */

        char* direction = data[0];
        char* msg_type = data[1];
        char* method = data[2];
        char* reason_phrase = data[3];
        char* warn_text = data[4];

        DBG("%s: SIP Registration info indication (sipRegInfoInd):"
            " account id: %d, response code: %d, direction: %s, msg type: %s, method: %s, reason phrase: %s, warn text: %s",
            self->slot, account_id, response_code, direction, msg_type, method, reason_phrase, warn_text);
        g_strfreev(data);
    } else {
        DBG("%s: failed to parse sipRegInfoInd data", self->slot);
    }
}

static
void
mtk_radio_ext_handle_ims_reg_info_ind(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    gsize count = 0, size = 0;
    const int32_t *data;

    /* imsRegInfoInd(RadioIndicationType type, vec<int32_t> info) */
    gbinder_reader_copy(&reader, args);
    data = gbinder_reader_read_hidl_vec(&reader, &count, &size);

    if (data) {
        /* 1. reg_state
         * 2. reg_type
         * 3. ext_info
         * 4. dereg_cause
         * 5. ims_retry
         * 6. rat
         * 7. sip_uri_type */
        int reg_state = 0, reg_type = 0, ext_info = 0, dereg_cause = 0, ims_retry = 0, rat = 0, sip_uri_type = 0;

        reg_state = data[0];
        reg_type = data[1];
        ext_info = data[2];
        dereg_cause = data[3];
        ims_retry = data[4];
        rat = data[5];
        sip_uri_type = data[6];

        DBG("%s: IMS registration info indication (imsRegInfoInd):"
            " reg state: %d, reg type: %d, ext info: %d, dereg cause: %d, ims retry: %d, rat: %d, sip uri type: %d",
            self->slot, reg_state, reg_type, ext_info, dereg_cause, ims_retry, rat, sip_uri_type);
    } else {
        DBG("%s: failed to parse imsRegInfoInd data", self->slot);
    }
}

static
void
mtk_radio_ext_handle_rtt_capability_indication(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    gbinder_reader_copy(&reader, args);
    int call_id = 0, local_cap = 0, remote_cap = 0, local_status = 0, remote_status = 0;

    /* rttCapabilityIndication(RadioIndicationType type, int32_t callId, int32_t localCap,
                               int32_t remoteCap, int32_t localStatus, int32_t remoteStatus) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &call_id);
    gbinder_reader_read_int32(&reader, &local_cap);
    gbinder_reader_read_int32(&reader, &remote_cap);
    gbinder_reader_read_int32(&reader, &local_status);
    gbinder_reader_read_int32(&reader, &remote_status);

    DBG("%s: RTT Capability (rttCapabilityIndication): call id: %d, local capability: %d,"
         " remote capability: %d, local status: %d, remote status: %d",
         self->slot, call_id, local_cap, remote_cap, local_status, remote_status);
}

static
void
mtk_radio_ext_handle_send_vops_indication(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    gbinder_reader_copy(&reader, args);
    int vops = 0;

    /* sendVopsIndication(RadioIndicationType type, int32_t vops) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &vops);

    DBG("%s: VoPS Indication (sendVopsIndication): VoPS: %d", self->slot, vops);
}

static
void
mtk_radio_ext_handle_sip_call_progress_indicator(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;

    /* sipCallProgressIndicator(RadioIndicationType type, string callId, string dir,
                                string sipMsgType, string method, string responseCode, string reasonText) */
    gbinder_reader_copy(&reader, args);
    const char* call_id = gbinder_reader_read_hidl_string_c(&reader);
    const char* dir = gbinder_reader_read_hidl_string_c(&reader);
    const char* sip_msg_type = gbinder_reader_read_hidl_string_c(&reader);
    const char* method = gbinder_reader_read_hidl_string_c(&reader);
    const char* response_code = gbinder_reader_read_hidl_string_c(&reader);
    const char* reason_text = gbinder_reader_read_hidl_string_c(&reader);

    DBG("%s: SIP Call progress indicator (sipCallProgressIndicator): call id: %s, dir: %s,"
        " SIP message type: %s, method: %s, response code: %s, reason text: %s",
        self->slot, call_id, dir, sip_msg_type, method, response_code, reason_text);
}

static
void
mtk_radio_ext_handle_video_capability_indicator(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;

    /* videoCapabilityIndicator(RadioIndicationType type, string callId,
                                string localVideoCap, string remoteVideoCap) */
    gbinder_reader_copy(&reader, args);
    const char* call_id = gbinder_reader_read_hidl_string_c(&reader);
    const char* local_video_cap = gbinder_reader_read_hidl_string_c(&reader);
    const char* remote_video_cap = gbinder_reader_read_hidl_string_c(&reader);

    DBG("%s: Video capability indicator (videoCapabilityIndicator): call id: %s,"
        " local video capibility: %s, remote video capability: %s",
        self->slot, call_id, local_video_cap, remote_video_cap);
}

static
void
mtk_radio_ext_handle_on_xui(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;

    /* onXui(RadioIndicationType type, string accountId, string broadcastFlag, string xuiInfo) */
    gbinder_reader_copy(&reader, args);
    const char* account_id = gbinder_reader_read_hidl_string_c(&reader);
    const char* broadcast_flag = gbinder_reader_read_hidl_string_c(&reader);
    const char* xui_info = gbinder_reader_read_hidl_string_c(&reader);

    DBG("%s: XUI (onXui): account id: %s, broadcast flag: %s, xui info: %s",
        self->slot, account_id, broadcast_flag, xui_info);
}

static
void
mtk_radio_ext_handle_on_volte_subscription(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    int status = 0;

    /* onVolteSubscription(RadioIndicationType type, int32_t status) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &status);

    DBG("%s: VoLTE Subscription (onVolteSubscription): status: %d (%s)", self->slot, status,
        status == 1 ? "VoLTE card" : (status == 2 ? "non VoLTE card" : "Unknown"));
}

static
void
mtk_radio_ext_handle_call_rat_indication(
    MtkRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    int domain = 0, rat = 0;

    /* callRatIndication(RadioIndicationType type, int32_t domain, int32_t rat) */
    gbinder_reader_copy(&reader, args);
    gbinder_reader_read_int32(&reader, &domain);
    gbinder_reader_read_int32(&reader, &rat);

    DBG("%s: Call RAT (callRatIndication): domain: %d (%s), rat: %d (%s)", self->slot,
        domain, domain == 0 ? "CS" : (domain == 1 ? "IMS" : "Unknown"),
        rat, rat == 1 ? "LTE" : (rat == 2 ? "Wifi" : "Unknown"));
}

static
GBinderLocalReply*
mtk_radio_ext_indication(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    MtkRadioExt* self = THIS(user_data);
    const char* iface = gbinder_remote_request_interface(req);
    GBinderReader args;

    gbinder_remote_request_init_reader(req, &args);
    mtk_radio_ext_log_ind(self, code);
    mtk_radio_ext_dump_data(&args);

    if (g_str_equal(iface, MTK_RADIO_IMS_INDICATION)) {
        guint type;
        if (gbinder_reader_read_uint32(&args, &type)) {
            switch(code) {
            case IMS_RADIO_IND_INCOMING_CALL_INDICATION:
                mtk_radio_ext_handle_incoming_call_indication(self, &args);
                return NULL;
            case IMS_RADIO_IND_CALL_INFO_INDICATION:
                mtk_radio_ext_handle_call_info_indication(self, &args);
                return NULL;
            case IMS_RADIO_IND_SIP_CALL_PROGRESS_INDICATOR:
                mtk_radio_ext_handle_sip_call_progress_indicator(self, &args);
                return NULL;
            case IMS_RADIO_IND_VIDEO_CAPABILITY_INDICATOR:
                mtk_radio_ext_handle_video_capability_indicator(self, &args);
                return NULL;
            case IMS_RADIO_IND_ON_XUI:
                mtk_radio_ext_handle_on_xui(self, &args);
                return NULL;
            case IMS_RADIO_IND_ON_VOLTE_SUBSCRIPTION:
                mtk_radio_ext_handle_on_volte_subscription(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_REGISTRATION_INFO:
                mtk_radio_ext_handle_ims_registration_info(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_BEARER_INIT:
                mtk_radio_ext_handle_ims_bearer_init(self, &args);
                return NULL;
            case IMS_RADIO_IND_SPEECH_CODEC_INFO_INDICATION:
                mtk_radio_ext_handle_speech_codec_info_indication(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_CFG_FEATURE_CHANGED:
                mtk_radio_ext_handle_ims_cfg_feature_changed(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_CFG_DYNAMIC_IMS_SWITCH_COMPLETE:
                mtk_radio_ext_handle_ims_cfg_dynamic_ims_switch_complete(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_CFG_CONFIG_CHANGED:
                mtk_radio_ext_handle_ims_cfg_config_changed(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_CFG_CONFIG_LOADED:
                mtk_radio_ext_handle_ims_cfg_config_loaded(self, &args);
                return NULL;
            case IMS_RADIO_IND_RTT_CAPABILITY_INDICATION:
                mtk_radio_ext_handle_rtt_capability_indication(self, &args);
                return NULL;
            case IMS_RADIO_IND_SEND_VOPS_INDICATION:
                mtk_radio_ext_handle_send_vops_indication(self, &args);
                return NULL;
            case IMS_RADIO_IND_CALL_RAT_INDICATION:
                mtk_radio_ext_handle_call_rat_indication(self, &args);
                return NULL;
            case IMS_RADIO_IND_SIP_REG_INFO_IND:
                mtk_radio_ext_handle_sip_reg_info_ind(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_REG_STATUS_REPORT:
                mtk_radio_ext_handle_ims_reg_status_report(self, &args);
                return NULL;
            case IMS_RADIO_IND_IMS_REG_INFO_IND:
                mtk_radio_ext_handle_ims_reg_info_ind(self, &args);
                return NULL;
            }
        } else {
            DBG("Failed to decode IMS indication %s %u", iface, code);
            *status = GBINDER_STATUS_FAILED;
        }
        // TODO: ack type == RADIO_IND_ACK_EXP indications
    } else if (g_str_equal(iface, MTK_RADIO_MTK_INDICATION)) {
        guint type;
        if (gbinder_reader_read_uint32(&args, &type)) {
            switch(code) {
            default:
                DBG("Unhandled MTK indication code: %u", code);
                break;
            }
        } else {
            DBG("Failed to decode MTK indication %s %u", iface, code);
            *status = GBINDER_STATUS_FAILED;
        }
        // TODO: ack type == RADIO_IND_ACK_EXP indications
    }

    DBG("Unexpected indication %s %u", iface, code);
    *status = GBINDER_STATUS_FAILED;

    return NULL;
}

static
GBinderLocalReply*
mtk_radio_ext_response(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    MtkRadioExt* self = THIS(user_data);
    const char* iface = gbinder_remote_request_interface(req);
    GBinderReader reader;
    const RadioResponseInfo* info;

    gbinder_remote_request_init_reader(req, &reader);

    info = gbinder_reader_read_hidl_struct(&reader, RadioResponseInfo);
    mtk_radio_ext_log_resp(self, code, info ? info->serial : 0);
    mtk_radio_ext_dump_data(&reader);

    if (info && info->serial) {
        MtkRadioExtRequest* req = g_hash_table_lookup(self->requests,
            KEY(info->serial));

        if (req && req->response_code == code) {
            g_object_ref(self);
            if (req->handle_response) {
                req->handle_response(req, info, &reader);
            }
            g_hash_table_remove(self->requests, KEY(info->serial));
            g_object_unref(self);
        } else {
            if (req) {
                /* responses like setImsCfgFeatureValue or setImsCfg don't actually have anything to return (they just return RadioError if available)
                   this will cause a lot of false positives because we assume a request is always available (else its unexpected)
                   but this is not the cause, at least not when response doesn't return anything but a possible error message (which goes for a lot of IMS methods!)
                   so lets only mark the status as failed if a request payload does exist and we really don't have a response for it */
                DBG("Unexpected response %s %u, expected response code is %d", iface, code, req->response_code);
                *status = GBINDER_STATUS_FAILED;
            }
        }
    } else {
        DBG("Failed to parse RadioResponseInfo %s %u", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }

    return NULL;
}

static
GBinderLocalReply*
mtk_radio_ext_atci_response(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    GBinderReader reader;
    const AtciResponseInfo* info;
    const void *vec_data;
    gsize count, elem_size;
    MtkRadioExt* self = THIS(user_data);

    gbinder_remote_request_init_reader(req, &reader);
    info = gbinder_reader_read_hidl_struct(&reader, AtciResponseInfo);
    if (!info) {
        DBG("Failed to parse AtciResponseInfo");
        *status = GBINDER_STATUS_FAILED;
        return NULL;
    }

    vec_data = gbinder_reader_read_hidl_vec(&reader, &count, &elem_size);
    if (!vec_data) {
        DBG("Failed to read ATCI response data");
        *status = GBINDER_STATUS_FAILED;
        return NULL;
    }

    /* Hand off response processing to the D-Bus extension */
    if (!dbus_ext_handle_atci_response(self, info->serial, vec_data, count)) {
        DBG("Failed to handle ATCI response through D-Bus extension");
        *status = GBINDER_STATUS_FAILED;
    } else {
        DBG("ATCI response successfully passed to D-Bus extension");
        *status = GBINDER_STATUS_OK;
    }

    return NULL;
}

static
GBinderLocalReply*
mtk_radio_ext_atci_indication(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    /* We do not care about ATCI indications (nor do we know even when they are sent) */
    return NULL;
}

static
void
mtk_radio_ext_result_response(
    MtkRadioExtRequest* req,
    const RadioResponseInfo* info,
    const GBinderReader* args)
{
    MtkRadioExt* self = req->radio;
    MtkRadioExtResultRequest* result_req = G_CAST(req,
        MtkRadioExtResultRequest, base);

    if (result_req->complete) {
        // responses without RadioResponseInfo get discarded earlier
        result_req->complete(self, info->error, req->user_data);
    }
}

static
void
mtk_radio_ext_request_default_free(
    MtkRadioExtRequest* req)
{
    if (req->destroy) {
        req->destroy(req->user_data);
    }
    g_free(req);
}

static
void
mtk_radio_ext_request_destroy(
    gpointer user_data)
{
    MtkRadioExtRequest* req = user_data;

    gbinder_client_cancel(req->radio->client, req->tx);
    req->free(req);
}

static
gpointer
mtk_radio_ext_request_alloc(
    MtkRadioExt* self,
    gint32 resp,
    MtkRadioExtRequestHandlerFunc handler,
    GDestroyNotify destroy,
    void* user_data,
    gsize size)
{
    MtkRadioExtRequest* req = g_malloc0(size);

    req->radio = self;
    req->response_code = resp;
    req->handle_response = handler;
    req->id = mtk_radio_ext_new_req_id();
    req->free = mtk_radio_ext_request_default_free;
    req->destroy = destroy;
    req->user_data = user_data;
    g_hash_table_insert(self->requests, KEY(req->id), req);
    return req;
}

static
MtkRadioExtResultRequest*
mtk_radio_ext_result_request_new(
    MtkRadioExt* self,
    gint32 resp,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    MtkRadioExtResultRequest* req =
        (MtkRadioExtResultRequest*)mtk_radio_ext_request_alloc(self, resp,
            mtk_radio_ext_result_response, destroy, user_data,
            sizeof(MtkRadioExtResultRequest));

    req->complete = complete;
    return req;
}

static
void
mtk_radio_ext_request_sent(
    GBinderClient* client,
    GBinderRemoteReply* reply,
    int status,
    void* user_data)
{
    ((MtkRadioExtRequest*)user_data)->tx = 0;
}

static
gulong
mtk_radio_ext_submit_request(
    MtkRadioExtRequest* request,
    gint32 code,
    gint32 serial,
    GBinderLocalRequest* args)
{
    return (request->tx = mtk_radio_ext_call(request->radio,
        code, serial, args, mtk_radio_ext_request_sent, NULL, request));
}

static
guint
mtk_radio_ext_result_request_submit(
    MtkRadioExt* self,
    gint32 req_code,
    gint32 resp_code,
    MtkRadioExtArgWriteFunc write_args,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data,
    ...)
{
    if (G_LIKELY(self)) {
        GBinderLocalRequest* args;
        GBinderWriter writer;
        MtkRadioExtResultRequest* req =
            mtk_radio_ext_result_request_new(self, resp_code,
                complete, destroy, user_data);
        const guint req_id = req->base.id;

        args = gbinder_client_new_request2(self->client, req_code);
        gbinder_local_request_init_writer(args, &writer);
        gbinder_writer_append_int32(&writer, req_id);
        if (write_args) {
            va_list va;

            va_start(va, user_data);
            write_args(&writer, va);
            va_end(va);
        }

        /* Submit the request */
        mtk_radio_ext_submit_request(&req->base, req_code, req_id, args);
        gbinder_local_request_unref(args);
        if (req->base.tx) {
            /* Success */
            return req_id;
        }
        g_hash_table_remove(self->requests, KEY(req_id));
    }
    return 0;
}

static
void
mtk_radio_ext_dispatch_at_command(
    MtkRadioExt* self,
    guint32 serial,
    const char* command)
{
    const guint code = MTK_RADIO_REQ_SEND_ATCI_REQUEST;
    GBinderLocalRequest* req;
    GBinderWriter writer;
    int status;

    req = gbinder_client_new_request2(self->client, code);
    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_int32(&writer, serial);
    gbinder_writer_append_hidl_vec(&writer, command, strlen(command), 1);

    status = gbinder_client_transact_sync_oneway(self->client, code, req);
    if (status != GBINDER_STATUS_OK) {
        DBG("Failed to send AT command %d", status);
    }

    gbinder_local_request_unref(req);
}

static
MtkRadioExt*
mtk_radio_ext_create(
    GBinderServiceManager* sm,
    GBinderRemoteObject* remote,
    const char* slot)
{
    MtkRadioExt* self = g_object_new(THIS_TYPE, NULL);
    const gint ims_code = MTK_RADIO_REQ_SET_RESPONSE_FUNCTIONS_IMS;
    const gint mtk_code = MTK_RADIO_REQ_SET_RESPONSE_FUNCTIONS_MTK;
    const gint atci_code = MTK_RADIO_REQ_SET_RESPONSE_FUNCTIONS_FOR_ATCI;
    GBinderLocalRequest *ims_req, *mtk_req, *atci_req;
    GBinderWriter ims_writer, mtk_writer, atci_writer;
    int status;

    self->slot = g_strdup(slot);
    self->client = gbinder_client_new(remote, MTK_RADIO);

    self->ims_response = gbinder_servicemanager_new_local_object(sm,
        MTK_RADIO_IMS_RESPONSE, mtk_radio_ext_response, self);
    self->ims_indication = gbinder_servicemanager_new_local_object(sm,
        MTK_RADIO_IMS_INDICATION, mtk_radio_ext_indication, self);
    self->atci_response = gbinder_servicemanager_new_local_object(sm,
        MTK_RADIO_ATCI_RESPONSE, mtk_radio_ext_atci_response, self);
    self->atci_indication = gbinder_servicemanager_new_local_object(sm,
        MTK_RADIO_ATCI_INDICATION, mtk_radio_ext_atci_indication, self);

    /* IMtkRadioEx:setResponseFunctionsIms */
    ims_req = gbinder_client_new_request2(self->client, ims_code);
    gbinder_local_request_init_writer(ims_req, &ims_writer);
    gbinder_writer_append_local_object(&ims_writer, self->ims_response);
    gbinder_writer_append_local_object(&ims_writer, self->ims_indication);

    mtk_radio_ext_log_req(self, ims_code, 0 /*serial*/);
    mtk_radio_ext_dump_request(ims_req);
    gbinder_remote_reply_unref(gbinder_client_transact_sync_reply(self->client,
        ims_code, ims_req, &status));

    DBG("setResponseFunctionsIms status %d", status);
    gbinder_local_request_unref(ims_req);

    self->mtk_response = gbinder_servicemanager_new_local_object(sm,
        MTK_RADIO_MTK_RESPONSE, mtk_radio_ext_response, self);
    self->mtk_indication = gbinder_servicemanager_new_local_object(sm,
        MTK_RADIO_MTK_INDICATION, mtk_radio_ext_indication, self);

    /* IMtkRadioEx:setResponseFunctionsMtk */
    mtk_req = gbinder_client_new_request2(self->client, mtk_code);
    gbinder_local_request_init_writer(mtk_req, &mtk_writer);
    gbinder_writer_append_local_object(&mtk_writer, self->mtk_response);
    gbinder_writer_append_local_object(&mtk_writer, self->mtk_indication);

    mtk_radio_ext_log_req(self, mtk_code, 0 /*serial*/);
    mtk_radio_ext_dump_request(mtk_req);
    gbinder_remote_reply_unref(gbinder_client_transact_sync_reply(self->client,
        mtk_code, mtk_req, &status));

    DBG("setResponseFunctionsMtk status %d", status);
    gbinder_local_request_unref(mtk_req);

    /* IMtkRadioEx:setResponseFunctionsForAtci */
    atci_req = gbinder_client_new_request2(self->client, atci_code);
    gbinder_local_request_init_writer(atci_req, &atci_writer);
    gbinder_writer_append_local_object(&atci_writer, self->atci_response);
    gbinder_writer_append_local_object(&atci_writer, self->atci_indication);

    mtk_radio_ext_log_req(self, atci_code, 0 /*serial*/);
    mtk_radio_ext_dump_request(atci_req);
    gbinder_remote_reply_unref(gbinder_client_transact_sync_reply(self->client,
        atci_code, atci_req, &status));

    DBG("setResponseFunctionsForAtci status %d", status);
    gbinder_local_request_unref(atci_req);

    /* Initialize D-Bus extension */
    if (!dbus_ext_init(self, slot)) {
        DBG("Failed to initialize D-Bus extension for slot %s", slot);
    }

    /* Register AT command dispatch function */
    dbus_ext_register_dispatch_func(self, mtk_radio_ext_dispatch_at_command);

    return self;
}

/* Taken and modified from ofono-binder-plugin's binder_sms.c */

static
void
mtk_ims_sms_gsm_message(
    GBinderWriter* writer,
    RadioGsmSmsMessage* msg,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len,
    const GBinderParent* parent)
{
    char* tpdu;
    int smsc_len = pdu_len - tpdu_len;
    guint msg_index;

    binder_copy_hidl_string(writer, &msg->smscPdu, NULL);

    /* PDU is sent as an ASCII hex string */
    msg->pdu.len = tpdu_len * 2;
    tpdu = gbinder_writer_malloc(writer, msg->pdu.len + 1);
    ofono_encode_hex(pdu + smsc_len, tpdu_len, tpdu);
    msg->pdu.data.str = tpdu;
    DBG("PDU: %s", tpdu);

    /* Write GsmSmsMessage and its strings */
    msg_index = gbinder_writer_append_buffer_object_with_parent(writer,
        msg, sizeof(*msg), parent);
    binder_append_hidl_string_data(writer, msg, smscPdu, msg_index);
    binder_append_hidl_string_data(writer, msg, pdu, msg_index);
}

static
void
mtk_ims_sms_ims_message(
    GBinderWriter* writer,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len)
{
    RadioImsSmsMessage* ims = gbinder_writer_new0(writer, RadioImsSmsMessage);
    RadioGsmSmsMessage* gsm = gbinder_writer_new0(writer, RadioGsmSmsMessage);
    GBinderParent p;

    ims->tech = RADIO_TECH_FAMILY_3GPP2;
    ims->gsmMessage.count = 1;
    ims->gsmMessage.data.ptr = gsm;
    ims->gsmMessage.owns_buffer = TRUE;

    p.index = gbinder_writer_append_buffer_object(writer, ims, sizeof(*ims));
    p.offset = G_STRUCT_OFFSET(RadioImsSmsMessage, cdmaMessage.data.ptr);
    gbinder_writer_append_buffer_object_with_parent(writer, NULL, 0, &p);

    p.offset = G_STRUCT_OFFSET(RadioImsSmsMessage, gsmMessage.data.ptr);
    mtk_ims_sms_gsm_message(writer, gsm, pdu, pdu_len, tpdu_len, &p);
}

/*==========================================================================*
 * API
 *==========================================================================*/

MtkRadioExt*
mtk_radio_ext_new(
    const char* dev,
    const char* slot)
{
    MtkRadioExt* self = NULL;
    GBinderServiceManager* sm = gbinder_servicemanager_new(dev);

    if (sm) {
        char* fqname = g_strconcat(MTK_RADIO, "/", slot, NULL);
        GBinderRemoteObject* obj = /* autoreleased */
            gbinder_servicemanager_get_service_sync(sm, fqname, NULL);

        if (obj) {
            DBG("Connected to %s", fqname);
            self = mtk_radio_ext_create(sm, obj, slot);
        } else {
            DBG("Failed to connect to %s", fqname);
        }

        g_free(fqname);
        gbinder_servicemanager_unref(sm);
    }

    return self;
}

MtkRadioExt*
mtk_radio_ext_ref(
    MtkRadioExt* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return self;
}

void
mtk_radio_ext_unref(
    MtkRadioExt* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

void
mtk_radio_ext_cancel(
    MtkRadioExt* self,
    guint id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_hash_table_remove(self->requests, KEY(id));
    }
}

static
void
mtk_radio_ext_set_enabled_args(
    GBinderWriter* args,
    va_list va)
{
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
}

guint
mtk_radio_ext_set_enabled(
    MtkRadioExt* self,
    gboolean enabled,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return mtk_radio_ext_result_request_submit(self,
        MTK_RADIO_REQ_SET_IMS_ENABLED,
        IMS_RADIO_RESP_SET_IMS_ENABLED,
        mtk_radio_ext_set_enabled_args,
        complete, destroy, user_data,
        enabled);
}

static
void
mtk_radio_ext_set_ims_cfg_feature_value_args(
    GBinderWriter* args,
    va_list va)
{
    // featureId
    gbinder_writer_append_int32(args, va_arg(va, guint32));
    // network
    gbinder_writer_append_int32(args, va_arg(va, guint32));
    // value
    gbinder_writer_append_int32(args, va_arg(va, guint32));
    // isLast
    gbinder_writer_append_int32(args, va_arg(va, guint32));
}

guint
mtk_radio_ext_set_ims_cfg_feature_value(
    MtkRadioExt* self,
    guint32 feature_id,
    guint32 network,
    guint32 value,
    guint32 is_last,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return mtk_radio_ext_result_request_submit(self,
        MTK_RADIO_REQ_SET_IMS_CFG_FEATURE_VALUE,
        IMS_RADIO_RESP_SET_IMS_CFG_FEATURE_VALUE,
        mtk_radio_ext_set_ims_cfg_feature_value_args,
        complete, destroy, user_data,
        feature_id, network, value, is_last);
}

static
void
mtk_radio_ext_set_ims_cfg_args(
    GBinderWriter* args,
    va_list va)
{
    // volteEnable
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
    // vilteEnable
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
    // vowifiEnable
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
    // viwifiEnable
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
    // smsEnable
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
    // eimsEnable
    gbinder_writer_append_bool(args, va_arg(va, gboolean));
}

/*
    Note: currently unused in favor of setImsCfgFeatureValue request,
    but keept in case it's needed again.

    From analyzing MtkImsService, it is used only during emergency calls
    and sends the AT+EIMS command to modem directly, temporary overriding
    the IMS settings.

    In normal mode, MTK RIL would eventually send another AT+EIMS command
    with the settings stored in properties, effectively overwriting
    the values set using setImsCfg.
 */
guint
mtk_radio_ext_set_ims_cfg(
    MtkRadioExt* self,
    gboolean volte_enable,
    gboolean vilte_enable,
    gboolean vowifi_enable,
    gboolean viwifi_enable,
    gboolean sms_enable,
    gboolean eims_enable,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return mtk_radio_ext_result_request_submit(self,
        MTK_RADIO_REQ_SET_IMS_CFG,
        IMS_RADIO_RESP_SET_IMS_CFG,
        mtk_radio_ext_set_ims_cfg_args,
        complete, destroy, user_data,
        volte_enable, vilte_enable, vowifi_enable,
        viwifi_enable, sms_enable, eims_enable);
}

static
void
mtk_radio_ext_set_wifi_enabled_args(
    GBinderWriter* args,
    va_list va)
{
    // ifname
    gbinder_writer_append_hidl_string(args, va_arg(va, char*));
    // isWifiEnabled
    gbinder_writer_append_int32(args, va_arg(va, guint32));
    // isFlightModeOn
    gbinder_writer_append_int32(args, va_arg(va, guint32));
}

guint
mtk_radio_ext_set_wifi_enabled(
    MtkRadioExt* self,
    char* ifname,
    guint32 is_wifi_enabled,
    guint32 is_flight_mode_on,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return mtk_radio_ext_result_request_submit(self,
        MTK_RADIO_REQ_SET_WIFI_ENABLED,
        0, // sendWifiEnabledResponse is in IMtkRadioExResponse which we do not subscribe to (we don't really need to either), give a dummy value for now
        mtk_radio_ext_set_wifi_enabled_args,
        complete, destroy, user_data,
        ifname, is_wifi_enabled, is_flight_mode_on);
}

static
void
mtk_radio_ext_set_wifi_ip_address_args(
    GBinderWriter* args,
    va_list va)
{
    const char* ifName = va_arg(va, const char*);
    const char* ipv4Addr = va_arg(va, const char*);
    const char* ipv4PrefixLen = va_arg(va, const char*);
    const char* ipv4Gateway = va_arg(va, const char*);
    const char* dnsCount = va_arg(va, const char*);
    const char* dnsServers = va_arg(va, const char*);

    const char* data[] = {
        ifName,
        ipv4Addr,
        "",  // ipv6Addr (we don't set a listener for ipv6)
        ipv4PrefixLen,
        "",  // ipv6PrefixLen (we don't set a listener for ipv6)
        ipv4Gateway,
        "",  // ipv6Gateway (we don't set a listener for ipv6)
        dnsCount,
        dnsServers
    };

    gssize count = sizeof(data) / sizeof(data[0]);

    DBG("ifName: %s, ipv4Addr: %s, ipv4PrefixLen: %s, ipv4Gateway: %s, dnsCount: %s, dnsServers: %s",
        ifName, ipv4Addr, ipv4PrefixLen, ipv4Gateway, dnsCount, dnsServers);

    // data <ipv4Addr, ipv6Addr, ipv4PrefixLen, ipv6PrefixLen, ipv4Gateway, ipv6Gateway, dnsCount, dnsServers>
    gbinder_writer_append_hidl_string_vec(args, data, count);
}

guint
mtk_radio_ext_set_wifi_ip_address(
    MtkRadioExt* self,
    const char* iface,
    const char* ipv4_addr,
    guint32 ipv4_prefix_len,
    const char* ipv4_gateway,
    guint32 dns_count,
    const char* dns_servers,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    char ipv4_prefix_len_str[16];
    char dns_count_str[16];

    snprintf(ipv4_prefix_len_str, sizeof(ipv4_prefix_len_str), "%u", ipv4_prefix_len);
    snprintf(dns_count_str, sizeof(dns_count_str), "%u", dns_count);

    return mtk_radio_ext_result_request_submit(self,
        MTK_RADIO_REQ_SET_WIFI_IP_ADDRESS,
        0, // sendWifiIpAddressResponse is in IMtkRadioExResponse which we do not subscribe to, give a dummy value for now
        mtk_radio_ext_set_wifi_ip_address_args,
        complete, destroy, user_data,
        iface, ipv4_addr, ipv4_prefix_len_str,
        ipv4_gateway, dns_count_str, dns_servers);
}

static
void
mtk_radio_ext_hangup_all_args(
    GBinderWriter* args,
    va_list va)
{
}

guint
mtk_radio_ext_hangup_all(
    MtkRadioExt* self,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return mtk_radio_ext_result_request_submit(self,
        MTK_RADIO_REQ_HANGUP_ALL,
        IMS_RADIO_RESP_HANGUP_ALL,
        mtk_radio_ext_hangup_all_args,
        complete, destroy, user_data);
}

guint
mtk_radio_ext_send_ims_sms_ex(
    MtkRadioExt* self,
    const char* smsc,
    const void* pdu,
    gsize pdu_len,
    MtkRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        const guint code = MTK_RADIO_REQ_SEND_IMS_SMS_EX;
        const guint resp_code = IMS_RADIO_RESP_SEND_IMS_SMS_EX;
        GBinderClient* client = self->client;
        GBinderLocalRequest* args = gbinder_client_new_request2(client, code);
        GBinderWriter writer;
        MtkRadioExtResultRequest* req =
            mtk_radio_ext_result_request_new(self, resp_code,
                complete, destroy, user_data);
        const guint req_id = req->base.id;

        gbinder_local_request_init_writer(args, &writer);
        gbinder_writer_append_int32(&writer, req_id);
        mtk_ims_sms_ims_message(&writer, pdu, pdu_len, pdu_len - (smsc ? strlen(smsc) : 0));

        /* Submit the request */
        mtk_radio_ext_submit_request(&req->base, code, req_id, args);
        gbinder_local_request_unref(args);
        if (req->base.tx) {
            /* Success */
            return req_id;
        }
        g_hash_table_remove(self->requests, KEY(req_id));
    }
    return 0;
}

gulong
mtk_radio_ext_add_ims_reg_status_handler(
    MtkRadioExt* self,
    MtkRadioExtImsRegStatusFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_IMS_REG_STATUS_CHANGED_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
mtk_radio_ext_add_ims_registration_info_handler(
    MtkRadioExt* self,
    MtkRadioExtImsRegistrationInfoFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_IMS_REGISTRATION_INFO_CHANGED_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
mtk_radio_ext_add_call_info_handler(
    MtkRadioExt* self,
    MtkRadioExtCallInfoFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_CALL_INFO_NAME, G_CALLBACK(handler), user_data) : 0;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
mtk_radio_ext_finalize(
    GObject* object)
{
    MtkRadioExt* self = THIS(object);

    dbus_ext_cleanup(self);

    g_free(self->slot);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
mtk_radio_ext_init(
    MtkRadioExt* self)
{
    self->pool = gutil_idle_pool_new();
    self->requests = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
        mtk_radio_ext_request_destroy);
}

static
void
mtk_radio_ext_class_init(
    MtkRadioExtClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = mtk_radio_ext_finalize;
    mtk_radio_ext_signals[SIGNAL_IMS_REG_STATUS_CHANGED] =
        g_signal_new(SIGNAL_IMS_REG_STATUS_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            1, G_TYPE_UINT);
    mtk_radio_ext_signals[SIGNAL_IMS_REGISTRATION_INFO_CHANGED] =
        g_signal_new(SIGNAL_IMS_REGISTRATION_INFO_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            2, G_TYPE_INT, G_TYPE_INT);
    mtk_radio_ext_signals[SIGNAL_CALL_INFO] =
        g_signal_new(SIGNAL_CALL_INFO_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            4, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
