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
 * Copyright (C) 2013 Aleksander Morgado <aleksander@gnu.org>
 * Copyright (C) 2017 Tempered Networks Inc.
 * Copyright (C) 2020 Packet Digital LLC
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem-at.h"
#include "mm-broadband-bearer-telit-ncm.h"
#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-telit.h"

G_DEFINE_TYPE (MMBroadbandBearerTelitNcm, mm_broadband_bearer_telit_ncm, MM_TYPE_BROADBAND_BEARER)

/*****************************************************************************/
/* 3GPP Dialing (sub-step of the 3GPP Connection sequence) */

typedef enum {
    DIAL_3GPP_CONTEXT_STEP_FIRST = 0,
    DIAL_3GPP_CONTEXT_STEP_SET_NCM,
    DIAL_3GPP_CONTEXT_STEP_SET_CGACT,
    DIAL_3GPP_CONTEXT_STEP_SET_CGDATA,
    DIAL_3GPP_CONTEXT_STEP_LAST
} Dial3gppContextStep;

typedef struct {
    MMBroadbandBearerTelitNcm   *self;
    MMBaseModem                 *modem;
    MMPortSerialAt              *primary;
    MMPortSerialAt              *secondary;
    guint                       cid;
    MMBearerIpFamily            ip_family;
    MMPort                      *data;
    GPtrArray *                 creg_regexes;
    Dial3gppContextStep         step;
} Dial3gppContext;



static void
dial_3gpp_context_free (Dial3gppContext *ctx)
{
    g_object_unref (ctx->self);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->secondary);
    g_object_unref (ctx->data);
    g_slice_free (Dial3gppContext, ctx);
}

/* Forward Function declarations */
static void dial_3gpp_context_step (GTask *task);

static MMPort *
dial_3gpp_finish (MMBroadbandBearer  *self,
                  GAsyncResult       *res,
                  GError            **error)
{
    return MM_PORT (g_task_propagate_pointer (G_TASK (res), error));
}

/* Generic AT command result ready callback */
static void
dial_3gpp_res_ready (MMBaseModem *modem,
                     GAsyncResult *res,
                     GTask *task)
{
    Dial3gppContext *ctx;
    GError          *error = NULL;

    ctx = (Dial3gppContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go to next step */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void set_reg_msg_handlers (Dial3gppContext *ctx, gboolean enable)
{
    guint i;

    if (!ctx->creg_regexes)
    {
        ctx->creg_regexes = mm_3gpp_creg_regex_get(FALSE);
    }
    for (i = 0; i < ctx->creg_regexes->len; i++)
    {
        mm_port_serial_at_enable_unsolicited_msg_handler (ctx->primary, (GRegex *)ctx->creg_regexes->pdata[i], enable);
        mm_port_serial_at_enable_unsolicited_msg_handler (ctx->secondary, (GRegex *)ctx->creg_regexes->pdata[i], enable);
    }
    if (enable)
        mm_dbg("Enabling unsolicited registration message handlers.");
    else
        mm_dbg("Disabling unsolicited registration message handlers.");
}

static void
dial_3gpp_context_step (GTask *task)
{
    Dial3gppContext *ctx;

    ctx = (Dial3gppContext *) g_task_get_task_data (task);

    mm_dbg("Dial3goo Step : %d", ctx->step);

    /* Check for cancellation */
    if (g_task_return_error_if_cancelled (task)) {
        set_reg_msg_handlers(ctx, TRUE);
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case DIAL_3GPP_CONTEXT_STEP_FIRST:
    case DIAL_3GPP_CONTEXT_STEP_SET_NCM: {
        gchar *command;
        command = g_strdup_printf ("#NCM=1,%u", ctx->cid);
        mm_base_modem_at_command (ctx->modem,
                                  command,
                                  10,
                                  FALSE,
                                  (GAsyncReadyCallback)dial_3gpp_res_ready,
                                  task);
        g_free (command);
        return;
    }
    case DIAL_3GPP_CONTEXT_STEP_SET_CGACT: {
        gchar *command;
        command = g_strdup_printf ("+CGACT=1,%u", ctx->cid);
        mm_base_modem_at_command (ctx->modem,
                                  command,
                                  30,
                                  FALSE,
                                  (GAsyncReadyCallback)dial_3gpp_res_ready,
                                  task);
        g_free (command);
        return;

    }
    case DIAL_3GPP_CONTEXT_STEP_SET_CGDATA: {
        gchar *command;
        command = g_strdup_printf ("+CGDATA=\"M-RAW_IP\",%u", ctx->cid);
        mm_base_modem_at_command (ctx->modem,
                                  command,
                                  10,
                                  FALSE,
                                  (GAsyncReadyCallback)dial_3gpp_res_ready,
                                  task);
        g_free (command);
        return;
    }
    case DIAL_3GPP_CONTEXT_STEP_LAST: {
        /* Restore message handlers */
        set_reg_msg_handlers(ctx, TRUE);

        g_task_return_pointer (task, g_object_ref (ctx->data), g_object_unref);
        g_object_unref (task);
        return;
    }
    }

}

static void
dial_3gpp (MMBroadbandBearer *self,
           MMBaseModem *modem,
           MMPortSerialAt *primary,
           guint cid,
           GCancellable *cancellable,
           GAsyncReadyCallback callback,
           gpointer user_data)
{
    GTask               *task;
    Dial3gppContext     *ctx;

    g_assert (primary != NULL);

    /* Setup task */
    task = g_task_new (self, cancellable, callback, user_data);
    ctx = g_slice_new0 (Dial3gppContext);
    g_task_set_task_data (task, ctx, (GDestroyNotify) dial_3gpp_context_free);

    /* Setup Context */
    ctx->self = g_object_ref (self);
    ctx->modem = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cid = cid;
    ctx->step = DIAL_3GPP_CONTEXT_STEP_FIRST;
    ctx->creg_regexes = NULL;

    /* Get a net port for the connection */
    ctx->data = mm_base_modem_peek_best_data_port (MM_BASE_MODEM (modem), MM_PORT_TYPE_NET);
    if (!ctx->data) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                         "No valid data port found to launch connection");
        g_object_unref (task);
        return;
    }
    g_object_ref (ctx->data);

    ctx->secondary = mm_base_modem_get_port_secondary (MM_BASE_MODEM (modem));

    /* Run! */
    dial_3gpp_context_step (task);
}

/*****************************************************************************/
/* 3GPP IP config retrieval (sub-step of the 3GPP Connection sequence) */

typedef enum {
    GET_IP_CONFIG_3GPP_CONTEXT_STEP_CGPADDR,
    GET_IP_CONFIG_3GPP_CONTEXT_STEP_CGCONTRDP,
    GET_IP_CONFIG_3GPP_CONTEXT_STEP_LAST
} GetIPConfig3gppContextStep;

typedef struct {
    MMBroadbandBearerTelitNcm   *self;
    MMBaseModem                 *modem;
    MMPortSerialAt              *primary;
    guint                       cid;
    MMBearerIpFamily            ip_family;
    MMPort                      *data;
    GetIPConfig3gppContextStep  step;
    MMBearerIpConfig            *ipv4_config;
    MMBearerIpConfig            *ipv6_config;
} GetIPConfig3gppContext;

static void
get_ip_config_3gpp_context_free (GetIPConfig3gppContext *ctx)
{
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->data);
    if (ctx->ipv4_config)
        g_object_unref (ctx->ipv4_config);
    if (ctx->ipv6_config)
        g_object_unref (ctx->ipv6_config);
}

/* forward declaration */
static void get_ip_config_3gpp_context_step (GTask *task);

static gboolean
get_ip_config_3gpp_finish (MMBroadbandBearer *self,
                           GAsyncResult *res,
                           MMBearerIpConfig **ipv4_config,
                           MMBearerIpConfig **ipv6_config,
                           GError **error)
{
    MMBearerConnectResult *configs;
    MMBearerIpConfig *ipv4;
    MMBearerIpConfig *ipv6;

    configs = g_task_propagate_pointer (G_TASK (res), error);
    if (!configs)
        return FALSE;

    g_assert(configs != NULL);

    /* IPv4 config */
    ipv4 = mm_bearer_connect_result_peek_ipv4_config (configs);
    if (ipv4)
        *ipv4_config = g_object_ref (ipv4);

    /* IPv6 config */
    ipv6 = mm_bearer_connect_result_peek_ipv6_config (configs);

    if (ipv6)
        *ipv6_config = g_object_ref (ipv6);

    mm_bearer_connect_result_unref (configs);
    return TRUE;
}

static void
ip_info_ready (MMBaseModem *modem,
               GAsyncResult *res,
               GTask *task)
{
    const gchar *response;
    GError *error = NULL;
    guint cid;
    gchar *ipv4addr = NULL;
    gchar *ipv6addr = NULL;
    GetIPConfig3gppContext *ctx;

    ctx = (GetIPConfig3gppContext *) g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Parse response */
    if (!mm_3gpp_parse_cgpaddr_write_response (response, &cid, &ipv4addr, &ipv6addr)) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Error parsing +CGPADDR response: '%s'",
                                 response);
        g_object_unref (task);
        return;
    }

    g_warn_if_fail (cid == ctx->cid);
    /* Create IPv4 config */
    if (ipv4addr)
    {
        ctx->ipv4_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ctx->ipv4_config, MM_BEARER_IP_METHOD_STATIC);
        mm_bearer_ip_config_set_address (ctx->ipv4_config, ipv4addr);
        mm_bearer_ip_config_set_prefix (ctx->ipv4_config, 0);
        g_free (ipv4addr);
    }
    /* IPv6 config */
    if (ipv6addr)
    {
        ctx->ipv6_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ctx->ipv6_config, MM_BEARER_IP_METHOD_STATIC);
        mm_bearer_ip_config_set_address (ctx->ipv6_config, ipv6addr);
        mm_bearer_ip_config_set_prefix (ctx->ipv6_config, 0);
        g_free (ipv6addr);
    }

    ctx->step++;
    get_ip_config_3gpp_context_step (task);
}

static void
dns_info_ready (MMBaseModem *modem,
                GAsyncResult *res,
                GTask *task)
{
    const gchar *response;
    GError *error = NULL;
    guint cid;
    guint bearer_id;
    gchar *apn = NULL;
    gchar *local_address = NULL;
    gchar *subnet = NULL;
    gchar *gateway_address = NULL;
    gchar *dns[3] = { 0 };
    GetIPConfig3gppContext *ctx;

    ctx = (GetIPConfig3gppContext *) g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Parse response */
    if (!mm_3gpp_parse_cgcontrdp_response (response, &cid, &bearer_id,
                                           &apn, &local_address, &subnet,
                                           &gateway_address,
                                           &dns[0],
                                           &dns[1],
                                           &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_warn_if_fail (cid == ctx->cid);

    if (dns[0])
        mm_bearer_ip_config_set_dns (ctx->ipv4_config, (const gchar **)dns);

    if (gateway_address)
        mm_bearer_ip_config_set_gateway (ctx->ipv4_config, gateway_address);

    if (subnet)
        mm_bearer_ip_config_set_prefix (ctx->ipv4_config, mm_netmask_to_cidr (subnet));

    g_free(apn);
    g_free(local_address);
    g_free(subnet);
    g_free(gateway_address);
    g_free(dns[0]);
    g_free(dns[1]);

    ctx->step++;
    get_ip_config_3gpp_context_step (task);
}

static void
get_ip_config_3gpp_context_step (GTask *task)
{
    GetIPConfig3gppContext *ctx;

    ctx = (GetIPConfig3gppContext *) g_task_get_task_data (task);

    mm_dbg("GetIPConfig Step : %d", ctx->step);

    switch (ctx->step) {
    case GET_IP_CONFIG_3GPP_CONTEXT_STEP_CGPADDR: {
        gchar *command = g_strdup_printf ("+CGPADDR=%u", ctx->cid);
        mm_base_modem_at_command (ctx->modem,
                                  command,
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)ip_info_ready,
                                  task);
        g_free (command);
        return;
    }
    case GET_IP_CONFIG_3GPP_CONTEXT_STEP_CGCONTRDP: {
        // TODO(cgrahn): Handle ipv4 and ipv6
        /* query DNS addresses */
        gchar *command = g_strdup_printf ("+CGCONTRDP=%u", ctx->cid);
        mm_base_modem_at_command (ctx->modem,
                                  command,
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)dns_info_ready,
                                  task);
        return;
    }
    case GET_IP_CONFIG_3GPP_CONTEXT_STEP_LAST: {

        g_task_return_pointer (task,
                           mm_bearer_connect_result_new (ctx->data, ctx->ipv4_config, ctx->ipv6_config),
                           (GDestroyNotify) mm_bearer_connect_result_unref);

        g_object_unref (task);
        return;
    }
    }
}

static void
get_ip_config_3gpp (MMBroadbandBearer *self,
                    MMBroadbandModem *modem,
                    MMPortSerialAt *primary,
                    MMPortSerialAt *secondary,
                    MMPort *data,
                    guint cid,
                    MMBearerIpFamily ip_family,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GTask                   *task;
    GetIPConfig3gppContext  *ctx;

    g_assert (primary != NULL);
    g_assert (data != NULL);
    g_assert (modem != NULL);

    /* Setup task and create disconnect context */
    task = g_task_new (self, NULL, callback, user_data);
    ctx = g_slice_new0 (GetIPConfig3gppContext);
    g_task_set_task_data (task, ctx, (GDestroyNotify) get_ip_config_3gpp_context_free);

    /* Setup context */
    ctx->self    = g_object_ref (self);
    ctx->modem   = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cid     = cid;
    ctx->ip_family = ip_family;
    ctx->data    = g_object_ref (data);
    ctx->step    = GET_IP_CONFIG_3GPP_CONTEXT_STEP_CGPADDR;
    ctx->ipv4_config = NULL;
    ctx->ipv6_config = NULL;

    /* Start */
    get_ip_config_3gpp_context_step (task);
}


/*****************************************************************************/
/* Disconnect 3GPP */

typedef enum {
    DISCONNECT_3GPP_CONTEXT_STEP_FIRST,
    DISCONNECT_3GPP_CONTEXT_STEP_NCMD,
    DISCONNECT_3GPP_CONTEXT_STEP_LAST
} Disconnect3gppContextStep;

typedef struct {
    MMBroadbandBearerTelitNcm  *self;
    MMBaseModem                *modem;
    MMPortSerialAt             *primary;
    MMPort                     *data;
    guint                       cid;
    Disconnect3gppContextStep   step;
} Disconnect3gppContext;

static void
disconnect_3gpp_context_free (Disconnect3gppContext *ctx)
{
    g_object_unref (ctx->self);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->data);
    g_slice_free (Disconnect3gppContext, ctx);
}

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer  *self,
                        GAsyncResult       *res,
                        GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/* forward declaration */
static void
disconnect_3gpp_context_step (GTask *task);

static void
cgact_disconnect_ready (MMBaseModem  *modem,
                        GAsyncResult *res,
                        GTask        *task)
{
    Disconnect3gppContext *ctx;
    GError *error = NULL;

    mm_base_modem_at_command_finish (modem, res, &error);

    if (error) {
        if(!g_error_matches (error,
                            MM_CONNECTION_ERROR,
                            MM_CONNECTION_ERROR_NO_CARRIER))
        {
        mm_dbg ("PDP context deactivation failed (not fatal): %s", error->message);
        }
        g_error_free (error);
    }

    ctx = (Disconnect3gppContext *) g_task_get_task_data (task);

    /* Go on to next step */
    ctx->step++;
    disconnect_3gpp_context_step (task);
}

static void
disconnect_3gpp_context_step (GTask *task)
{
    Disconnect3gppContext *ctx;

    ctx = (Disconnect3gppContext *) g_task_get_task_data (task);

    mm_dbg("Disconnect3gpp Step : %d", ctx->step);

    switch (ctx->step) {
    case DISCONNECT_3GPP_CONTEXT_STEP_FIRST:
    case DISCONNECT_3GPP_CONTEXT_STEP_NCMD: {
        mm_base_modem_at_command (ctx->modem,
                                  "#NCMD=0",
                                  30,
                                  FALSE,
                                  (GAsyncReadyCallback) cgact_disconnect_ready,
                                  task);
        return;
    }
    case DISCONNECT_3GPP_CONTEXT_STEP_LAST:{
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    }
}

static void
disconnect_3gpp (MMBroadbandBearer  *self,
                 MMBroadbandModem   *modem,
                 MMPortSerialAt     *primary,
                 MMPortSerialAt     *secondary,
                 MMPort             *data,
                 guint               cid,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
    GTask                 *task;
    Disconnect3gppContext *ctx;

    g_assert (primary != NULL);
    g_assert (data != NULL);
    g_assert (modem != NULL);

    /* Setup task and create disconnect context */
    task = g_task_new (self, NULL, callback, user_data);
    ctx = g_slice_new0 (Disconnect3gppContext);
    g_task_set_task_data (task, ctx, (GDestroyNotify) disconnect_3gpp_context_free);

    /* Setup context */
    ctx->self    = g_object_ref (self);
    ctx->modem   = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->data    = g_object_ref (data);
    ctx->cid     = cid;
    ctx->step    = DISCONNECT_3GPP_CONTEXT_STEP_FIRST;

    /* Start */
    disconnect_3gpp_context_step (task);
}

/*****************************************************************************/

MMBaseBearer *
mm_broadband_bearer_telit_ncm_new_finish (GAsyncResult *res,
                                          GError **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    /* Only export valid bearers */
    mm_base_bearer_export (MM_BASE_BEARER (bearer));

    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_telit_ncm_new (MMBroadbandModemTelit *modem,
                                   MMBearerProperties *config,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_TELIT_NCM,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM, modem,
        MM_BASE_BEARER_CONFIG, config,
        NULL);
}

static void
mm_broadband_bearer_telit_ncm_init (MMBroadbandBearerTelitNcm *self)
{

    return;
}

static void
mm_broadband_bearer_telit_ncm_class_init (MMBroadbandBearerTelitNcmClass *klass)
{
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    broadband_bearer_class->dial_3gpp = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish = dial_3gpp_finish;

    broadband_bearer_class->get_ip_config_3gpp = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;


    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;
}
