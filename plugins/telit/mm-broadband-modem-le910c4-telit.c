/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2018 Aleksander Morgado <aleksander@aleksander.es>
 * Copyright (C) 2020 Packet Digital LLC
 */

#include <config.h>

#include "mm-broadband-modem-le910c4-telit.h"
#include "mm-log.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers-telit.h"
#include "mm-shared-qmi.h"
#include "mm-shared-telit.h"

/* index to use with AT#FWSWITCH to select firmware */
#define NON_VERIZON_FIRMWARE_INDEX 0
#define VERIZON_FIRMWARE_INDEX 1

/* List of mccmnc for Verizon from https://www.mcc-mnc.com/ */
static const gchar * const verizon_list[] = {
    "311273",
    "311289",
    "311278",
    "311483",
    "310004",
    "311283",
    "311488",
    "310890",
    "311272",
    "311288",
    "311277",
    "311482",
    "311282",
    "311487",
    "310590",
    "311271",
    "311287",
    "311276",
    "311481",
    "311281",
    "311486",
    "310013",
    "311270",
    "311286",
    "311275",
    "311480",
    "311280",
    "311485",
    "310012",
    "311110",
    "311285",
    "311274",
    "311390",
    "311279",
    "311484",
    "310010",
    "311284",
    "311489",
    "310910",
};

static void
mm_firmware_change_register_task_telit_start (MMIfaceModem3gpp    *self,
                                              const gchar         *operator_id,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer            user_data);

static void
set_current_bands (MMIfaceModem        *self,
                   GArray              *bands_array,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data);

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);
static void shared_telit_init (MMSharedTelit *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemLe910c4Telit, mm_broadband_modem_le910c4_telit, MM_TYPE_BROADBAND_MODEM_QMI, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_TELIT, shared_telit_init));

/*****************************************************************************/

MMBroadbandModemLe910c4Telit *
mm_broadband_modem_le910c4_telit_new (const gchar  *device,
                                      const gchar **drivers,
                                      const gchar  *plugin,
                                      guint16       vendor_id,
                                      guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_LE910C4_TELIT,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_le910c4_telit_init (MMBroadbandModemLe910c4Telit *self)
{
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->set_current_bands = set_current_bands;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface->register_in_network = mm_firmware_change_register_task_telit_start;
}

static void
shared_telit_init (MMSharedTelit *iface)
{
}

static void
mm_broadband_modem_le910c4_telit_class_init (MMBroadbandModemLe910c4TelitClass *klass)
{
}

/*****************************************************************************/
/* Create Bearer (Modem interface) */

typedef enum {
    FIRMWARE_CHANGE_REGISTER_STEP_FIRST,
    FIRMWARE_CHANGE_REGISTER_STEP_CHECK_FIRMWARE,
    FIRMWARE_CHANGE_REGISTER_STEP_CHECK_IMSI,
    FIRMWARE_CHANGE_REGISTER_STEP_LAST,
} CreateBearerStep;

typedef struct {
    MMIfaceModem3gpp    *self;
    CreateBearerStep     step;
    gboolean             verizon_firmware_loaded;
    const gchar         *operator_id;
    GCancellable        *cancellable;
    GAsyncReadyCallback callback;
    gpointer            user_data;
} FirmwareChangeRegisterContext;

static void
firmware_change_register_context_free (FirmwareChangeRegisterContext *ctx)
{
    g_object_unref (ctx->self);
    g_slice_free (FirmwareChangeRegisterContext, ctx);
}

static void firmware_change_register_step (GTask *task);

static void
firmware_check_ready (MMBaseModem  *self,
                      GAsyncResult *res,
                      GTask        *task)
{
    const gchar *response;
    GError *error = NULL;
    FirmwareChangeRegisterContext *ctx;
    gint firmware_index, storage_type;

    ctx = (FirmwareChangeRegisterContext *) g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response) {
        mm_warn ("telit: couldn't read current firmware in use: %s", error->message);
        g_error_free (error);
    } else if (!mm_telit_parse_fwswitch_response (response, &firmware_index, &storage_type, &error)) {
        mm_warn ("telit: couldn't parse fwswitch response '%s': %s", response, error->message);
        g_error_free (error);
    } else {
        mm_info ("telit: firmware index %d, storage type %d",
                 firmware_index, storage_type);
        ctx->verizon_firmware_loaded = (firmware_index == VERIZON_FIRMWARE_INDEX);
    }

    ctx->step++;
    firmware_change_register_step (task);
}

static const gchar *
read_imsi (MMIfaceModem  *self,
           GError **error)
{
    GError    *inner_error = NULL;
    MMBaseSim *sim = NULL;
    g_object_get (self,
                  MM_IFACE_MODEM_SIM, &sim,
                  NULL);

    if (!sim) {
        mm_warn ("telit: SIM not found");
        inner_error = g_error_new (MM_CORE_ERROR,
                                   MM_CORE_ERROR_FAILED,
                                   "Couldn't retrieve SIM object for Telit modem.");

    }
    else {
        const char *imsi;
        imsi = mm_gdbus_sim_get_imsi (MM_GDBUS_SIM (sim));
        if (!imsi) {
            mm_warn ("telit: Unable to get IMSI from SIM");
            inner_error = g_error_new (MM_CORE_ERROR,
                                       MM_CORE_ERROR_FAILED,
                                       "Couldn't read IMSI from SIM object.");
        } else {
            mm_dbg ( "telit: imsi is %s", imsi);
            g_object_unref (sim);
            return imsi;
        }
        g_object_unref (sim);
    }

    if (inner_error) {
        g_propagate_error(error, inner_error);
        g_prefix_error(error, "Failed to get IMSI. ");
        return NULL;
    }

    return NULL;
}

static gboolean
is_verizon_sim (const gchar *imsi)
{
    guint i;
    for (i = 0; i < G_N_ELEMENTS (verizon_list); i++) {
        if (g_str_has_prefix (imsi, verizon_list[i])) {
            mm_dbg ("telit: found Verizon SIM. imsi is %s, mccmnc is %s",
                     imsi, verizon_list[i]);
            return TRUE;
        }
    }
    mm_dbg ("telit: did not find Verizon SIM");
    return FALSE;
}

static void
change_firmware_ready (MMBaseModem  *self,
                       GAsyncResult *res,
                       GTask        *task)
{
    const gchar *response;
    GError *error = NULL;
    FirmwareChangeRegisterContext *ctx;

    ctx = (FirmwareChangeRegisterContext *) g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response) {
        mm_warn ("telit: failed to change firmware: %s", error->message);
        g_error_free (error);
    } else {
        mm_info("telit: firmware change successfully requested. "
                "Wait for modem to restart");
        // abort task here as the modem is going to reboot
        // and ModemManager will need to reenumerate the modem
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Modem firmware change in progress. "
                             "Please wait for modem to reboot.");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->step++;
    firmware_change_register_step (task);
}

static gboolean
change_firmware (MMBaseModem *self,
                 GTask *task,
                 const gchar *imsi,
                 gboolean verizon_firmware_loaded,
                 GError **error)
{
    gboolean verizon_sim;
    verizon_sim = is_verizon_sim (imsi);
    if (verizon_sim != verizon_firmware_loaded) {
        gchar *cmd;
        gint firmware_index = verizon_sim ? VERIZON_FIRMWARE_INDEX :
            NON_VERIZON_FIRMWARE_INDEX;
        const gint non_volatile_storage = 1;
        cmd = g_strdup_printf ("AT#FWSWITCH=%d,%d", firmware_index,
                               non_volatile_storage);

        mm_info ("telit: changing firmware to index %d. Modem will restart...",
                 firmware_index);
        mm_base_modem_at_command (
            self,
            cmd,
            5,
            FALSE,
            (GAsyncReadyCallback) change_firmware_ready,
            task);

        return TRUE;
    }

    return FALSE;
}

static void
firmware_change_register_step (GTask *task)
{
    FirmwareChangeRegisterContext *ctx;

    ctx = (FirmwareChangeRegisterContext *) g_task_get_task_data (task);
    switch (ctx->step) {
    case FIRMWARE_CHANGE_REGISTER_STEP_FIRST:
        ctx->step++;
        /* fall down */

    case FIRMWARE_CHANGE_REGISTER_STEP_CHECK_FIRMWARE:
        mm_dbg ("telit: checking current firmware...");
        mm_base_modem_at_command (
            MM_BASE_MODEM (ctx->self),
            "AT#FWSWITCH?",
            3,
            FALSE,
            (GAsyncReadyCallback) firmware_check_ready,
            task);
        return;

    case FIRMWARE_CHANGE_REGISTER_STEP_CHECK_IMSI: {
        GError      *error = NULL;
        const gchar *imsi = read_imsi (MM_IFACE_MODEM (ctx->self), &error);
        if (imsi) {
            if (!change_firmware (MM_BASE_MODEM (ctx->self), task, imsi,
                                  ctx->verizon_firmware_loaded, &error))
                mm_info ("telit: no firmware change needed");
            else {
                mm_info ("telit: firmware change requested");
                return;
            }

        } else {
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }
    }
        ctx->step++;
        /* fall down */

    case FIRMWARE_CHANGE_REGISTER_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert_not_reached ();
}

static gboolean
bearer_task_telit_finish (MMIfaceModem3gpp  *self,
                          GAsyncResult      *res,
                          GError           **error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);

    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
firmware_steps_done (GObject  *source_object,
                     GAsyncResult      *res,
                     gpointer          data)
{
    MMIfaceModem3gpp *modem = (MMIfaceModem3gpp *) source_object;
    FirmwareChangeRegisterContext * ctx;
    gboolean success;
    GError *error = NULL;

    success = bearer_task_telit_finish (modem, res, &error);

    if (success)
    {
        ctx = (FirmwareChangeRegisterContext *) data;

        mm_shared_qmi_3gpp_register_in_network (ctx->self,
                                                ctx->operator_id,
                                                ctx->cancellable,
                                                ctx->callback,
                                                ctx->user_data);
    } else {
        mm_warn ("telit: Got error registering: %s", error->message);
        g_error_free (error);
    }
}

static void
mm_firmware_change_register_task_telit_start (MMIfaceModem3gpp    *self,
                                              const gchar         *operator_id,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer            user_data)
{
    FirmwareChangeRegisterContext *ctx;
    GTask                         *task;

    mm_dbg ("telit: Starting firmware change task");
    ctx = g_slice_new0 (FirmwareChangeRegisterContext);
    ctx->self = g_object_ref (self);
    ctx->step = FIRMWARE_CHANGE_REGISTER_STEP_FIRST;
    ctx->verizon_firmware_loaded = FALSE;
    ctx->operator_id = operator_id;
    ctx->cancellable = cancellable;
    ctx->callback = callback;
    ctx->user_data = user_data;

    task = g_task_new (self, NULL, firmware_steps_done, ctx);
    g_task_set_task_data (task, ctx, (GDestroyNotify) firmware_change_register_context_free);
    firmware_change_register_step (task);
}

static void
set_current_bands (MMIfaceModem        *self,
                   GArray              *bands_array,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    /* Disable Telit AUTOBND when setting bands manually */
    mm_shared_telit_modem_disable_autoband (self,
                                            bands_array,
                                            callback,
                                            mm_shared_qmi_set_current_bands,
                                            user_data);
}
