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
 * Copyright (C) 2019 Daniele Palmas <dnlplm@gmail.com>
 */

#include <config.h>

#include <stdio.h>

#include <glib-object.h>
#include <gio/gio.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-location.h"
#include "mm-base-modem.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers-telit.h"
#include "mm-shared-telit.h"

/*****************************************************************************/
/* Load current mode (Modem interface) */

gboolean
mm_shared_telit_load_current_modes_finish (MMIfaceModem *self,
                                           GAsyncResult *res,
                                           MMModemMode *allowed,
                                           MMModemMode *preferred,
                                           GError **error)
{
    const gchar *response;
    const gchar *str;
    gint a;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response)
        return FALSE;

    str = mm_strip_tag (response, "+WS46: ");

    if (!sscanf (str, "%d", &a)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Couldn't parse +WS46 response: '%s'",
                     response);
        return FALSE;
    }

    *preferred = MM_MODEM_MODE_NONE;
    switch (a) {
    case 12:
        *allowed = MM_MODEM_MODE_2G;
        return TRUE;
    case 22:
        *allowed = MM_MODEM_MODE_3G;
        return TRUE;
    case 25:
        if (mm_iface_modem_is_3gpp_lte (self))
            *allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
        else
            *allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        return TRUE;
    case 28:
        *allowed = MM_MODEM_MODE_4G;
        return TRUE;
    case 29:
        *allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        return TRUE;
    case 30:
        *allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_4G);
        return TRUE;
    case 31:
        *allowed = (MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
        return TRUE;
    default:
        break;
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "Couldn't parse unexpected +WS46 response: '%s'",
                 response);
    return FALSE;
}

void
mm_shared_telit_load_current_modes (MMIfaceModem *self,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+WS46?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Load supported bands (Modem interface) */

typedef struct {
    gboolean mm_modem_is_2g;
    gboolean mm_modem_is_3g;
    gboolean mm_modem_is_4g;
    MMTelitLoadBandsType band_type;
} LoadBandsContext;

static void
mm_shared_telit_load_bands_context_free (LoadBandsContext *ctx)
{
    g_slice_free (LoadBandsContext, ctx);
}

static void
mm_shared_telit_load_bands_ready (MMBaseModem *self,
                                  GAsyncResult *res,
                                  GTask *task)
{
    const gchar *response;
    GError *error = NULL;
    GArray *bands = NULL;
    LoadBandsContext *ctx;

    ctx = g_task_get_task_data (task);
    response = mm_base_modem_at_command_finish (self, res, &error);

    if (!response)
        g_task_return_error (task, error);
    else if (!mm_telit_parse_bnd_response (response,
                                           ctx->mm_modem_is_2g,
                                           ctx->mm_modem_is_3g,
                                           ctx->mm_modem_is_4g,
                                           ctx->band_type,
                                           &bands,
                                           &error))
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bands, (GDestroyNotify)g_array_unref);

    g_object_unref (task);
}

void
mm_shared_telit_modem_load_supported_bands (MMIfaceModem *self,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
    GTask *task;
    LoadBandsContext *ctx;

    ctx = g_slice_new0 (LoadBandsContext);

    ctx->mm_modem_is_2g = mm_iface_modem_is_2g (self);
    ctx->mm_modem_is_3g = mm_iface_modem_is_3g (self);
    ctx->mm_modem_is_4g = mm_iface_modem_is_4g (self);
    ctx->band_type = LOAD_SUPPORTED_BANDS;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)mm_shared_telit_load_bands_context_free);

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "#BND=?",
                              3,
                              FALSE,
                              (GAsyncReadyCallback) mm_shared_telit_load_bands_ready,
                              task);
}

/*****************************************************************************/
/* Load current bands (Modem interface) */

GArray *
mm_shared_telit_modem_load_bands_finish (MMIfaceModem *self,
                                         GAsyncResult *res,
                                         GError **error)
{
    return (GArray *) g_task_propagate_pointer (G_TASK (res), error);
}

void
mm_shared_telit_modem_load_current_bands (MMIfaceModem *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data)
{
    GTask *task;
    LoadBandsContext *ctx;

    ctx = g_slice_new0 (LoadBandsContext);

    ctx->mm_modem_is_2g = mm_iface_modem_is_2g (self);
    ctx->mm_modem_is_3g = mm_iface_modem_is_3g (self);
    ctx->mm_modem_is_4g = mm_iface_modem_is_4g (self);
    ctx->band_type = LOAD_CURRENT_BANDS;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)mm_shared_telit_load_bands_context_free);

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "#BND?",
                              3,
                              FALSE,
                              (GAsyncReadyCallback) mm_shared_telit_load_bands_ready,
                              task);
}

/*****************************************************************************/
/* Set current bands (Modem interface) */

gboolean
mm_shared_telit_modem_set_current_bands_finish (MMIfaceModem *self,
                                                GAsyncResult *res,
                                                GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
mm_shared_telit_modem_set_current_bands_ready (MMBaseModem *self,
                                               GAsyncResult *res,
                                               GTask *task)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (self, res, &error);
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);

    g_object_unref (task);
}

void
mm_shared_telit_modem_set_current_bands (MMIfaceModem *self,
                                         GArray *bands_array,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
    gchar *cmd;
    gint flag2g;
    gint flag3g;
    gint flag4g;
    gboolean is_2g;
    gboolean is_3g;
    gboolean is_4g;
    GTask *task;

    mm_telit_get_band_flag (bands_array, &flag2g, &flag3g, &flag4g);

    is_2g = mm_iface_modem_is_2g (self);
    is_3g = mm_iface_modem_is_3g (self);
    is_4g = mm_iface_modem_is_4g (self);

    if (is_2g && flag2g == -1) {
        g_task_report_new_error (self,
                                 callback,
                                 user_data,
                                 mm_shared_telit_modem_set_current_bands,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_NOT_FOUND,
                                 "None or invalid 2G bands combination in the provided list");
        return;
    }

    if (is_3g && flag3g == -1) {
        g_task_report_new_error (self,
                                 callback,
                                 user_data,
                                 mm_shared_telit_modem_set_current_bands,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_NOT_FOUND,
                                 "None or invalid 3G bands combination in the provided list");
        return;
    }

    if (is_4g && flag4g == -1) {
        g_task_report_new_error (self,
                                 callback,
                                 user_data,
                                 mm_shared_telit_modem_set_current_bands,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_NOT_FOUND,
                                 "None or invalid 4G bands combination in the provided list");
        return;
    }

    cmd = NULL;
    if (is_2g && !is_3g && !is_4g)
        cmd = g_strdup_printf ("AT#BND=%d", flag2g);
    else if (is_2g && is_3g && !is_4g)
        cmd = g_strdup_printf ("AT#BND=%d,%d", flag2g, flag3g);
    else if (is_2g && is_3g && is_4g)
        cmd = g_strdup_printf ("AT#BND=%d,%d,%d", flag2g, flag3g, flag4g);
    else if (!is_2g && !is_3g && is_4g)
        cmd = g_strdup_printf ("AT#BND=0,0,%d", flag4g);
    else if (!is_2g && is_3g && is_4g)
        cmd = g_strdup_printf ("AT#BND=0,%d,%d", flag3g, flag4g);
    else if (is_2g && !is_3g && is_4g)
        cmd = g_strdup_printf ("AT#BND=%d,0,%d", flag2g, flag4g);
    else {
        g_task_report_new_error (self,
                                 callback,
                                 user_data,
                                 mm_shared_telit_modem_set_current_bands,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Unexpected error: could not compose AT#BND command");
        return;
    }
    task = g_task_new (self, NULL, callback, user_data);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              cmd,
                              20,
                              FALSE,
                              (GAsyncReadyCallback)mm_shared_telit_modem_set_current_bands_ready,
                              task);
    g_free (cmd);
}

/*****************************************************************************/
/* Disable Telit AUTOBND (auto band selection) */

typedef enum {
    DISABLE_AUTOBAND_STEP_FIRST,
    DISABLE_AUTOBAND_STEP_CHECK_AUTOBND,
    DISABLE_AUTOBAND_STEP_DISABLE_AUTOBND,
    DISABLE_AUTOBAND_STEP_LAST,
} DisableAutobandStep;

typedef struct {
    MMIfaceModem                *self;
    DisableAutobandStep          step;
    GArray                      *bands_array;
    GAsyncReadyCallback          callback;
    MMSharedTelitSetCurrentBands set_current_bands;
    gpointer                     user_data;
} DisableAutobandContext;

static void
disable_autoband_step (GTask *task);

static void
disable_autoband_context_free (DisableAutobandContext *ctx)
{
    g_object_unref (ctx->self);
    g_slice_free (DisableAutobandContext, ctx);
}

static void
mm_shared_telit_modem_check_autoband_ready (MMBaseModem *self,
                                           GAsyncResult *res,
                                           GTask *task)
{
    const gchar *response;
    GError *error = NULL;
    DisableAutobandContext *ctx;

    ctx = (DisableAutobandContext *) g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (error) {
        mm_warn ("telit: failed to query if AUTOBND is enabled: %s",
                 error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_info ("telit: AUTOBND is %s", response);

    ctx->step++;
    disable_autoband_step (task);
}

static void
mm_shared_telit_modem_disable_autoband_ready (MMBaseModem *self,
                                             GAsyncResult *res,
                                             GTask *task)
{
    GError *error = NULL;
    DisableAutobandContext *ctx;

    ctx = (DisableAutobandContext *) g_task_get_task_data (task);

    mm_base_modem_at_command_finish (self, res, &error);
    if (error) {
        mm_warn ("telit: failed to disable AUTOBND: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_info ("telit: disabled AUTOBND");

    ctx->step++;
    disable_autoband_step (task);
}

static void
disable_autoband_step (GTask *task)
{
    DisableAutobandContext *ctx;

    ctx = (DisableAutobandContext *) g_task_get_task_data (task);
    switch (ctx->step) {
    case DISABLE_AUTOBAND_STEP_FIRST:
        ctx->step++;
        /* fall down */

    case DISABLE_AUTOBAND_STEP_CHECK_AUTOBND:
        mm_dbg ("telit: checking if AUTOBND is enabled...");
        mm_base_modem_at_command (MM_BASE_MODEM (ctx->self),
                                  "AT#AUTOBND?",
                                  20,
                                  FALSE,
                                  (GAsyncReadyCallback)mm_shared_telit_modem_check_autoband_ready,
                                  task);
        return;

    case DISABLE_AUTOBAND_STEP_DISABLE_AUTOBND:
        mm_dbg ("telit: disabling AUTOBND...");
        mm_base_modem_at_command (MM_BASE_MODEM (ctx->self),
                                  "AT#AUTOBND=0",
                                  20,
                                  FALSE,
                                  (GAsyncReadyCallback)mm_shared_telit_modem_disable_autoband_ready,
                                  task);
        return;

    case DISABLE_AUTOBAND_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert_not_reached ();
}

static gboolean
disable_autoband_task_finish (MMIfaceModem  *self,
                              GAsyncResult  *res,
                              GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);

    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disable_autoband_done (GObject  *source_object,
                       GAsyncResult      *res,
                       gpointer          data)
{
    MMIfaceModem *modem = (MMIfaceModem *) source_object;
    DisableAutobandContext * ctx;
    gboolean success;
    GError *error = NULL;

    success = disable_autoband_task_finish (modem, res, &error);

    if (success)
    {
        ctx = (DisableAutobandContext *) data;

        ctx->set_current_bands (ctx->self,
                                ctx->bands_array,
                                ctx->callback,
                                ctx->user_data);
    } else {
        mm_warn ("telit: Got error disabling AUTOBND: %s", error->message);
        g_error_free (error);
    }
}

void
mm_shared_telit_modem_disable_autoband (MMIfaceModem *self,
                                        GArray *bands_array,
                                        GAsyncReadyCallback callback,
                                        MMSharedTelitSetCurrentBands set_current_bands,
                                        gpointer user_data)
{
    DisableAutobandContext *ctx;
    GTask *task;
    ctx = g_slice_new0 (DisableAutobandContext);
    ctx->self = g_object_ref (self);
    ctx->step = DISABLE_AUTOBAND_STEP_FIRST;
    ctx->bands_array = bands_array;
    ctx->callback = callback;
    ctx->set_current_bands = set_current_bands;
    ctx->user_data = user_data;

    task = g_task_new (self, NULL, disable_autoband_done, ctx);
    g_task_set_task_data (task, ctx,
                          (GDestroyNotify) disable_autoband_context_free);
    disable_autoband_step (task);
}

/*****************************************************************************/
/* Set initial EPS bearer settings */
typedef enum {
    SET_INITIAL_EPS_STEP_FIRST,
    SET_INITIAL_EPS_STEP_CHECK_VERIZON_SIM,
    SET_INITIAL_EPS_STEP_CHANGE_APN,
    SET_INITIAL_EPS_STEP_LAST,
} SetInitialEpsBearerSettingsStep;

typedef struct {
    MMIfaceModem3gpp                *self;
    MMBearerProperties              *config;
    SetInitialEpsBearerSettingsStep  step;
} SetInitialEpsBearerSettingsContext;

static void
set_initial_eps_bearer_settings_step (GTask *task);

static void
set_initial_eps_bearer_settings_context_free (SetInitialEpsBearerSettingsContext *ctx)
{
    g_object_unref (ctx->self);
    g_object_unref (ctx->config);
    g_slice_free (SetInitialEpsBearerSettingsContext, ctx);
}

gboolean
mm_shared_telit_set_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                                        GAsyncResult      *res,
                                                        GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_initial_eps_bearer_apn_ready (MMBaseModem *self,
                                  GAsyncResult *res,
                                  GTask *task)
{
    SetInitialEpsBearerSettingsContext *ctx;
    GError             *error = NULL;
    const gchar        *apn;

    ctx = (SetInitialEpsBearerSettingsContext *) g_task_get_task_data (task);

    mm_base_modem_at_command_finish (self, res, &error);
    if (error) {
        mm_warn ("telit: failed to set intial EPS bearer settings: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    apn = mm_bearer_properties_get_apn (ctx->config);
    mm_info ("telit: set APN for initial EPS bearer to %s", apn);

    ctx->step++;
    set_initial_eps_bearer_settings_step (task);
}

static void
set_initial_eps_bearer_apn (GTask *task)
{
    SetInitialEpsBearerSettingsContext *ctx;
    const gchar          *apn;
    gchar                *quoted_apn;
    gchar                *cmd;
    /* TODO(cgrahn): Link to Telit documentation showing PDP CID 1 is for EPS bearer */
    const guint8          default_bearer_cid = 1;

    ctx = (SetInitialEpsBearerSettingsContext *) g_task_get_task_data (task);
    apn = mm_bearer_properties_get_apn (ctx->config);
    if (!apn) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "APN missing from config");
        g_object_unref (task);
        return;
    }

    quoted_apn = mm_port_serial_at_quote_string (apn);
    /* TODO(cgrahn): Don't hardcode pdptype ("IPV4V6")? */
    cmd = g_strdup_printf ("AT+CGDCONT=%u,\"%s\",%s", default_bearer_cid,
                           "IPV4V6", quoted_apn);
    g_free (quoted_apn);

    mm_base_modem_at_command (MM_BASE_MODEM (ctx->self),
                              cmd,
                              5,
                              FALSE,
                              (GAsyncReadyCallback)set_initial_eps_bearer_apn_ready,
                              task);
    g_free (cmd);
}

static void
set_initial_eps_bearer_settings_step (GTask *task)
{
    SetInitialEpsBearerSettingsContext *ctx;

    ctx = (SetInitialEpsBearerSettingsContext *) g_task_get_task_data (task);
    switch (ctx->step) {
    case SET_INITIAL_EPS_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_CHECK_VERIZON_SIM:
        ctx->step++;
        /* TODO */
        /* fall through */

    case SET_INITIAL_EPS_STEP_CHANGE_APN:
        set_initial_eps_bearer_apn (task);
        return;

    case SET_INITIAL_EPS_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

void
mm_shared_telit_set_initial_eps_bearer_settings (MMIfaceModem3gpp *self,
                                                 MMBearerProperties *config,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data)
{
    SetInitialEpsBearerSettingsContext *ctx;
    GTask                              *task;

    ctx = g_slice_new0 (SetInitialEpsBearerSettingsContext);
    ctx->self = g_object_ref (self);
    ctx->config = g_object_ref (config);
    ctx->step = SET_INITIAL_EPS_STEP_FIRST;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx,
                          (GDestroyNotify) set_initial_eps_bearer_settings_context_free);

    set_initial_eps_bearer_settings_step (task);
}

/*****************************************************************************/
/* Set current modes (Modem interface) */

gboolean
mm_shared_telit_set_current_modes_finish (MMIfaceModem *self,
                                          GAsyncResult *res,
                                          GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
mm_shared_telit_ws46_set_ready (MMBaseModem *self,
                                GAsyncResult *res,
                                GTask *task)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (self, res, &error);
    if (error)
        /* Let the error be critical. */
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_shared_telit_set_current_modes (MMIfaceModem *self,
                                   MMModemMode allowed,
                                   MMModemMode preferred,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
    GTask *task;
    gchar *command;
    gint ws46_mode = -1;

    task = g_task_new (self, NULL, callback, user_data);

    if (allowed == MM_MODEM_MODE_2G)
        ws46_mode = 12;
    else if (allowed == MM_MODEM_MODE_3G)
        ws46_mode = 22;
    else if (allowed == MM_MODEM_MODE_4G)
        ws46_mode = 28;
    else if (allowed == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G)) {
        if (mm_iface_modem_is_3gpp_lte (self))
            ws46_mode = 29;
        else
            ws46_mode = 25;
    } else if (allowed == (MM_MODEM_MODE_2G | MM_MODEM_MODE_4G))
        ws46_mode = 30;
    else if (allowed == (MM_MODEM_MODE_3G | MM_MODEM_MODE_4G))
        ws46_mode = 31;
    else if (allowed == (MM_MODEM_MODE_2G  | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G) ||
             allowed == MM_MODEM_MODE_ANY)
        ws46_mode = 25;

    /* Telit modems do not support preferred mode selection */
    if ((ws46_mode < 0) || (preferred != MM_MODEM_MODE_NONE)) {
        gchar *allowed_str;
        gchar *preferred_str;

        allowed_str = mm_modem_mode_build_string_from_mask (allowed);
        preferred_str = mm_modem_mode_build_string_from_mask (preferred);
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Requested mode (allowed: '%s', preferred: '%s') not "
                                 "supported by the modem.",
                                 allowed_str,
                                 preferred_str);
        g_free (allowed_str);
        g_free (preferred_str);

        g_object_unref (task);
        return;
    }

    command = g_strdup_printf ("AT+WS46=%d", ws46_mode);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        10,
        FALSE,
        (GAsyncReadyCallback)mm_shared_telit_ws46_set_ready,
        task);
    g_free (command);
}

/*****************************************************************************/

static void
shared_telit_init (gpointer g_iface)
{
}

GType
mm_shared_telit_get_type (void)
{
    static GType shared_telit_type = 0;

    if (!G_UNLIKELY (shared_telit_type)) {
        static const GTypeInfo info = {
            sizeof (MMSharedTelit),  /* class_size */
            shared_telit_init,       /* base_init */
            NULL,                  /* base_finalize */
        };

        shared_telit_type = g_type_register_static (G_TYPE_INTERFACE, "MMSharedTelit", &info, 0);
        g_type_interface_add_prerequisite (shared_telit_type, MM_TYPE_IFACE_MODEM);
    }

    return shared_telit_type;
}
