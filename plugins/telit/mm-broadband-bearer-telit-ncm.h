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
 */

#ifndef MM_BROADBAND_BEARER_TELIT_NCM_H
#define MM_BROADBAND_BEARER_TELIT_NCM_H

#include <glib.h>
#include <glib-object.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-bearer.h"
#include "mm-broadband-modem-telit.h"

#define MM_TYPE_BROADBAND_BEARER_TELIT_NCM            (mm_broadband_bearer_telit_ncm_get_type ())
#define MM_BROADBAND_BEARER_TELIT_NCM(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_BEARER_TELIT_NCM, MMBroadbandBearerTelitNcm))
#define MM_BROADBAND_BEARER_TELIT_NCM_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_BEARER_TELIT_NCM, MMBroadbandBearerTelitNcmClass))
#define MM_IS_BROADBAND_BEARER_TELIT_NCM(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_BEARER_TELIT_NCM))
#define MM_IS_BROADBAND_BEARER_TELIT_NCM_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_BEARER_TELIT_NCM))
#define MM_BROADBAND_BEARER_TELIT_NCM_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_BEARER_TELIT_NCM, MMBroadbandBearerTelitNcmClass))

typedef struct _MMBroadbandBearerTelitNcm MMBroadbandBearerTelitNcm;
typedef struct _MMBroadbandBearerTelitNcmClass MMBroadbandBearerTelitNcmClass;
typedef struct _MMBroadbandBearerTelitNcmPrivate MMBroadbandBearerTelitNcmPrivate;

struct _MMBroadbandBearerTelitNcm {
    MMBroadbandBearer parent;
    MMBroadbandBearerTelitNcmPrivate *priv;
};

struct _MMBroadbandBearerTelitNcmClass {
    MMBroadbandBearerClass parent;
};

GType mm_broadband_bearer_telit_ncm_get_type (void);

/* Default 3GPP bearer creation implementation */
void mm_broadband_bearer_telit_ncm_new (MMBroadbandModemTelit *modem,
                                        MMBearerProperties *config,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
MMBaseBearer *mm_broadband_bearer_telit_ncm_new_finish (GAsyncResult *res,
                                                        GError **error);

#endif /* MM_BROADBAND_BEARER_TELIT_NCM_H */
