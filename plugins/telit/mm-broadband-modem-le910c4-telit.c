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

#include "mm-log-object.h"
#include "mm-broadband-modem-le910c4-telit.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers-telit.h"
#include "mm-shared-qmi.h"
#include "mm-shared-telit.h"

/* index to use with AT#FWSWITCH to select firmware */
#define NON_VERIZON_FIRMWARE_INDEX 0
#define VERIZON_FIRMWARE_INDEX 1

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

static void
set_initial_eps_bearer_settings (MMIfaceModem3gpp *self,
                                 MMBearerProperties *config,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

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
    iface->modem_power_off = mm_shared_telit_modem_power_off;
    iface->modem_power_off_finish = mm_shared_telit_modem_power_off_finish;
    iface->set_current_bands = set_current_bands;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface->register_in_network = mm_firmware_change_register_task_telit_start;
    iface->set_initial_eps_bearer_settings = set_initial_eps_bearer_settings;
    iface->set_initial_eps_bearer_settings_finish = mm_shared_telit_set_initial_eps_bearer_settings_finish;
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
    MMBearerProperties  *config;
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
    if (ctx->config) {
        g_object_unref (ctx->config);
    }
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
        mm_obj_warn (self, "telit: couldn't read current firmware in use: %s", error->message);
        g_error_free (error);
    } else if (!mm_telit_parse_fwswitch_response (response, &firmware_index, &storage_type, &error)) {
        mm_obj_warn (self, "telit: couldn't parse fwswitch response '%s': %s", response, error->message);
        g_error_free (error);
    } else {
        mm_obj_info (self, "telit: firmware index %d, storage type %d",
               firmware_index, storage_type);
        ctx->verizon_firmware_loaded = (firmware_index == VERIZON_FIRMWARE_INDEX);
    }

    ctx->step++;
    firmware_change_register_step (task);
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
        mm_obj_warn (self, "telit: failed to change firmware: %s", error->message);
        g_error_free (error);
    } else {
        mm_obj_info (self, "telit: firmware change successfully requested. "
                     "Wait for modem to restart");
        // abort task here as the modem is going to reboot
        // and ModemManager will need to reenumerate the modem
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_ABORTED,
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
    verizon_sim = mm_shared_is_verizon_sim (imsi, self);
    if (verizon_sim != verizon_firmware_loaded) {
        gchar *cmd;
        gint firmware_index = verizon_sim ? VERIZON_FIRMWARE_INDEX :
            NON_VERIZON_FIRMWARE_INDEX;
        const gint non_volatile_storage = 1;
        cmd = g_strdup_printf ("AT#FWSWITCH=%d,%d", firmware_index,
                               non_volatile_storage);

        mm_obj_info (self, "telit: changing firmware to index %d. Modem will restart...",
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
        /* fall through */

    case FIRMWARE_CHANGE_REGISTER_STEP_CHECK_FIRMWARE:
        mm_obj_dbg (ctx->self, "telit: checking current firmware...");
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
        const gchar *imsi = mm_shared_telit_read_imsi (MM_IFACE_MODEM (ctx->self),
                                                       &error);
        if (imsi) {
            if (!change_firmware (MM_BASE_MODEM (ctx->self), task, imsi,
                                  ctx->verizon_firmware_loaded, &error))
                mm_obj_info (ctx->self, "telit: no firmware change needed");
            else {
                mm_obj_info (ctx->self, "telit: firmware change requested");
                return;
            }

        } else {
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }
    }
        ctx->step++;
        /* fall through */

    case FIRMWARE_CHANGE_REGISTER_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
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

    ctx = (FirmwareChangeRegisterContext *) data;
    if (g_task_had_error (G_TASK (res))) {
        /* Call the callback saved in ctx to propagate the error. */
        ctx->callback (source_object, res, ctx->user_data);
        return;
    }

    success = bearer_task_telit_finish (modem, res, &error);

    if (success)
    {
        mm_shared_qmi_3gpp_register_in_network (ctx->self,
                                                ctx->operator_id,
                                                ctx->cancellable,
                                                ctx->callback,
                                                ctx->user_data);
    } else {
        /* TODO(cgrahn): This branch is unreachable as firmware task
         * always returns either TRUE or an error. */
        mm_obj_warn (ctx->self, "telit: Got error registering: %s", error->message);
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

    mm_obj_dbg (self, "telit: Starting firmware change task");
    ctx = g_slice_new0 (FirmwareChangeRegisterContext);
    ctx->self = g_object_ref (self);
    ctx->config = NULL;
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

static void
set_initial_eps_bearer_settings_firmware_steps_done (GObject  *source_object,
                                                     GAsyncResult      *res,
                                                     gpointer          data)
{
    MMIfaceModem3gpp *modem = (MMIfaceModem3gpp *) source_object;
    FirmwareChangeRegisterContext *ctx;
    gboolean success;
    GError *error = NULL;

    ctx = (FirmwareChangeRegisterContext *) data;
    if (g_task_had_error (G_TASK (res))) {
        /* Call the callback saved in ctx to propagate the error. */
        ctx->callback (source_object, res, ctx->user_data);
        return;
    }

    success = bearer_task_telit_finish (modem, res, &error);

    if (success)
    {
        /* Modem is using correct firmware. Now we can set the APN for
         * the initial EPS bearer. */
        mm_shared_telit_set_initial_eps_bearer_settings (ctx->self,
                                                         ctx->config,
                                                         ctx->callback,
                                                         ctx->user_data);
    } else {
        /* TODO(cgrahn): This branch is unreachable as firmware task
         * always returns either TRUE or an error. */
        mm_obj_warn (ctx->self, "telit: Got error setting initial "
                     "EPS bearer settings: %s", error->message);
        g_error_free (error);
    }
}

/* This function makes sure the modem is using the correct firmware
 * before setting the APN for the initial EPS Bearer */
static void
set_initial_eps_bearer_settings (MMIfaceModem3gpp *self,
                                 MMBearerProperties *config,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    FirmwareChangeRegisterContext *ctx;
    GTask                         *task;

    mm_obj_dbg (self,
                "telit le910c4: Starting initial eps bearer settings task");
    ctx = g_slice_new0 (FirmwareChangeRegisterContext);
    ctx->self = g_object_ref (self);
    ctx->config = g_object_ref (config);
    ctx->step = FIRMWARE_CHANGE_REGISTER_STEP_FIRST;
    ctx->verizon_firmware_loaded = FALSE;
    ctx->operator_id = NULL;
    ctx->cancellable = NULL;
    ctx->callback = callback;
    ctx->user_data = user_data;

    task = g_task_new (self, NULL,
                       set_initial_eps_bearer_settings_firmware_steps_done,
                       ctx);
    g_task_set_task_data (task, ctx,
                          (GDestroyNotify) firmware_change_register_context_free);
    firmware_change_register_step (task);
}
