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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <glib-object.h>

#include "mtk_ims_sms.h"
#include "mtk_radio_ext.h"
#include "mtk_radio_ext_types.h"

#include <binder_ext_sms_impl.h>

#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>

#include <gutil_idlepool.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

typedef GObjectClass MtkImsSmsClass;
typedef struct mtk_ims_sms {
    GObject parent;
    GUtilIdlePool* pool;
    MtkRadioExt* radio_ext;
    RadioClient* ims_aosp_client;
    GPtrArray* sms;
} MtkImsSms;

typedef struct mtk_ims_sms_result_request {
    int ref_count;
    MtkImsSms* self;
    BinderExtSms* ext;
    BinderExtSmsSendFunc complete;
    GDestroyNotify destroy;
    void* user_data;
} MtkImsSmsResultRequest;

static
void
mtk_ims_sms_iface_init(
    BinderExtSmsInterface* iface);

GType mtk_ims_sms_get_type() G_GNUC_INTERNAL;
G_DEFINE_TYPE_WITH_CODE(MtkImsSms, mtk_ims_sms, G_TYPE_OBJECT,
G_IMPLEMENT_INTERFACE(BINDER_EXT_TYPE_SMS, mtk_ims_sms_iface_init))

#define THIS_TYPE mtk_ims_sms_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, MtkImsSms)
#define PARENT_CLASS mtk_ims_sms_parent_class

#define ID_KEY(id) GUINT_TO_POINTER(id)
#define ID_VALUE(id) GUINT_TO_POINTER(id)

enum mtk_ims_sms_signal {
    SIGNAL_SMS_STATE_CHANGED,
    SIGNAL_SMS_RECEIVED,
    SIGNAL_COUNT
};

#define SIGNAL_SMS_STATE_CHANGED_NAME    "mtk-ims-sms-state-changed"
#define SIGNAL_SMS_RECEIVED_NAME         "mtk-ims-sms-received"

static guint mtk_ims_sms_signals[SIGNAL_COUNT] = { 0 };

static
MtkImsSmsResultRequest*
mtk_ims_sms_result_request_new(
    MtkImsSms* self,
    BinderExtSmsSendFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    MtkImsSmsResultRequest* req =
        g_slice_new0(MtkImsSmsResultRequest);

    req->ref_count = 1;
    req->self = self;
    req->complete = complete;
    req->destroy = destroy;
    req->user_data = user_data;
    return req;
}

static
void
mtk_ims_sms_result_request_free(
    MtkImsSmsResultRequest* req)
{
    BinderExtSms* ext = req->ext;

    if (req->destroy) {
        req->destroy(req->user_data);
    }
    binder_ext_sms_unref(ext);
    gutil_slice_free(req);
}

static
gboolean
mtk_ims_sms_result_request_unref(
    MtkImsSmsResultRequest* req)
{
    if (!--(req->ref_count)) {
        mtk_ims_sms_result_request_free(req);
        return TRUE;
    } else {
        return FALSE;
    }
}

static
void
mtk_ims_sms_result_request_destroy(
    gpointer req)
{
    mtk_ims_sms_result_request_unref(req);
}

static
void
mtk_ims_sms_result_callback(
    MtkRadioExt* radio,
    int result,
    guint msg_ref,
    void* user_data)
{
    MtkImsSmsResultRequest* req = user_data;
    BINDER_EXT_SMS_SEND_RESULT send_result;

    if (result == RADIO_ERROR_NONE) {
        send_result = BINDER_EXT_SMS_SEND_RESULT_OK;
    } else {
        send_result = BINDER_EXT_SMS_SEND_RESULT_ERROR;
    }

    if (req->complete) {
        req->complete(BINDER_EXT_SMS(req->self), send_result, msg_ref,
            req->user_data);
    }
}


/*==========================================================================*
 * BinderExtSmsInterface
 *==========================================================================*/

static
guint
mtk_ims_sms_send(
    BinderExtSms* ext,
    const char* smsc,
    const void* pdu,
    gsize pdu_len,
    guint msg_ref,
    BINDER_EXT_SMS_SEND_FLAGS flags,
    BinderExtSmsSendFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    MtkImsSms* self = THIS(ext);
    guint ret = 0;
    MtkImsSmsResultRequest* req;

    DBG("Sending SMS over IMS: smsc=%s, pdu_len=%zu, msg_ref=%u", smsc, pdu_len, msg_ref);

    req = mtk_ims_sms_result_request_new(self, complete, destroy, user_data);

    ret = mtk_radio_ext_send_ims_sms_ex(self->radio_ext, smsc, pdu, pdu_len,
                                        mtk_ims_sms_result_callback,
                                        mtk_ims_sms_result_request_destroy,
                                        req);

    if (ret == 0) {
        mtk_ims_sms_result_request_unref(req);
    }

    return ret;
}

static
void
mtk_ims_sms_cancel(
    BinderExtSms* ext,
    guint id)
{
    DBG("cancel is not implemented yet");
}

static
void
mtk_ims_sms_ack_report(
    BinderExtSms* ext,
    guint msg_ref,
    gboolean ok)
{
    DBG("Acknowledging SMS report: msg_ref=%u, ok=%d", msg_ref, ok);
}

static
void
mtk_ims_sms_ack_incoming(
    BinderExtSms* ext,
    gboolean ok)
{
    DBG("Acknowledging incoming SMS: ok=%d", ok);
}

static
gulong
mtk_ims_sms_add_report_handler(
    BinderExtSms* ext,
    BinderExtSmsReportFunc handler,
    void* user_data)
{
    return g_signal_connect(ext, SIGNAL_SMS_STATE_CHANGED_NAME, G_CALLBACK(handler), user_data);
}

static
gulong
mtk_ims_sms_add_incoming_handler(
    BinderExtSms* ext,
    BinderExtSmsIncomingFunc handler,
    void* user_data)
{
    return g_signal_connect(ext, SIGNAL_SMS_RECEIVED_NAME, G_CALLBACK(handler), user_data);
}

static
void
mtk_ims_sms_remove_handler(
    BinderExtSms* ext,
    gulong id)
{
    g_signal_handler_disconnect(ext, id);
}

void
mtk_ims_sms_iface_init(
    BinderExtSmsInterface* iface)
{
    iface->flags |= BINDER_EXT_SMS_INTERFACE_FLAG_IMS_SUPPORT |
        BINDER_EXT_SMS_INTERFACE_FLAG_IMS_REQUIRED;
    iface->version = BINDER_EXT_SMS_INTERFACE_VERSION;
    iface->send = mtk_ims_sms_send;
    iface->cancel = mtk_ims_sms_cancel;
    iface->ack_report = mtk_ims_sms_ack_report;
    iface->ack_incoming = mtk_ims_sms_ack_incoming;
    iface->add_report_handler = mtk_ims_sms_add_report_handler;
    iface->add_incoming_handler = mtk_ims_sms_add_incoming_handler;
    iface->remove_handler = mtk_ims_sms_remove_handler;
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtSms*
mtk_ims_sms_new(
    MtkRadioExt* radio_ext,
    RadioClient* ims_aosp_client)
{
    if (G_LIKELY(radio_ext)) {
        MtkImsSms* self = g_object_new(THIS_TYPE, NULL);

        self->radio_ext = mtk_radio_ext_ref(radio_ext);
        self->ims_aosp_client = radio_client_ref(ims_aosp_client);
        self->sms = g_ptr_array_new_with_free_func(g_free);

        return BINDER_EXT_SMS(self);
    }
    return NULL;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
mtk_ims_sms_finalize(
    GObject* object)
{
    MtkImsSms* self = THIS(object);
    mtk_radio_ext_unref(self->radio_ext);
    radio_client_unref(self->ims_aosp_client);
    gutil_idle_pool_destroy(self->pool);
    g_ptr_array_free(self->sms, TRUE);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
mtk_ims_sms_init(
    MtkImsSms* self)
{
    self->pool = gutil_idle_pool_new();
}

static
void
mtk_ims_sms_class_init(
    MtkImsSmsClass* klass)
{
    GType type = G_OBJECT_CLASS_TYPE(klass);

    G_OBJECT_CLASS(klass)->finalize = mtk_ims_sms_finalize;
    mtk_ims_sms_signals[SIGNAL_SMS_STATE_CHANGED] =
        g_signal_new(SIGNAL_SMS_STATE_CHANGED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    mtk_ims_sms_signals[SIGNAL_SMS_RECEIVED] =
        g_signal_new(SIGNAL_SMS_RECEIVED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            2, G_TYPE_POINTER, G_TYPE_UINT);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
