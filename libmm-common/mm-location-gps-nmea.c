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
 * Copyright (C) 2012 Lanedo GmbH <aleksander@lanedo.com>
 */

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "mm-common-helpers.h"
#include "mm-errors-types.h"
#include "mm-location-gps-nmea.h"

G_DEFINE_TYPE (MMLocationGpsNmea, mm_location_gps_nmea, G_TYPE_OBJECT);

struct _MMLocationGpsNmeaPrivate {
    GHashTable *traces;
    GRegex *sequence_regex;
};

/*****************************************************************************/

static gboolean
check_append_or_replace (MMLocationGpsNmea *self,
                         const gchar *trace)
{
    /* By default, replace */
    gboolean append_or_replace = FALSE;
    GMatchInfo *match_info = NULL;

    if (G_UNLIKELY (!self->priv->sequence_regex))
        self->priv->sequence_regex = g_regex_new ("\\$GPGSV,(\\d),(\\d).*",
                                                  G_REGEX_RAW | G_REGEX_OPTIMIZE,
                                                  0,
                                                  NULL);

    if (g_regex_match (self->priv->sequence_regex, trace, 0, &match_info)) {
        guint index;

        /* If we don't have the first element of a sequence, append */
        if (mm_get_uint_from_match_info (match_info, 2, &index) && index != 1)
            append_or_replace = TRUE;
    }
    g_match_info_free (match_info);

    return append_or_replace;
}

static gboolean
location_gps_nmea_take_trace (MMLocationGpsNmea *self,
                              gchar *trace)
{
    gchar *i;
    gchar *trace_type;

    i = strchr (trace, ',');
    if (!i || i == trace)
        return FALSE;

    trace_type = g_malloc (i - trace + 1);
    memcpy (trace_type, trace, i - trace);
    trace_type[i - trace] = '\0';

    /* Some traces are part of a SEQUENCE; so we need to decide whether we
     * completely replace the previous trace, or we append the new one to
     * the already existing list */
    if (check_append_or_replace (self, trace)) {
        /* Append */
        const gchar *previous;
        gchar *sequence;

        previous = g_hash_table_lookup (self->priv->traces, trace_type);
        sequence = g_strdup_printf ("%s%s",
                                    previous ? previous : "",
                                    trace);
        g_free (trace);
        trace = sequence;
    }

    g_hash_table_replace (self->priv->traces,
                          trace_type,
                          trace);
    return TRUE;
}

gboolean
mm_location_gps_nmea_add_trace (MMLocationGpsNmea *self,
                                const gchar *trace)
{
    return location_gps_nmea_take_trace (self, g_strdup (trace));
}

/*****************************************************************************/

const gchar *
mm_location_gps_nmea_get_trace (MMLocationGpsNmea *self,
                                const gchar *trace_type)
{
    return (const gchar *)g_hash_table_lookup (self->priv->traces, trace_type);
}

/*****************************************************************************/

static void
build_full_foreach (const gchar *trace_type,
                    const gchar *trace,
                    GString **built)
{
    g_string_append (*built, trace);
}

gchar *
mm_location_gps_nmea_build_full (MMLocationGpsNmea *self)
{
    GString *built;

    built = g_string_new ("");
    g_hash_table_foreach (self->priv->traces,
                          (GHFunc)build_full_foreach,
                          &built);
    return g_string_free (built, FALSE);
}

/*****************************************************************************/

GVariant *
mm_location_gps_nmea_get_string_variant (MMLocationGpsNmea *self)
{
    GVariant *variant = NULL;
    gchar *built;

    g_return_val_if_fail (MM_IS_LOCATION_GPS_NMEA (self), NULL);

    built = mm_location_gps_nmea_build_full (self);
    variant = g_variant_new_string (built);
    g_free (built);

    return variant;
}

/*****************************************************************************/

MMLocationGpsNmea *
mm_location_gps_nmea_new_from_string_variant (GVariant *string,
                                              GError **error)
{
    MMLocationGpsNmea *self = NULL;
    gchar **split;
    guint i;

    if (!g_variant_is_of_type (string, G_VARIANT_TYPE_STRING)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_INVALID_ARGS,
                     "Cannot create GPS NMEA location from string: "
                     "invalid variant type received");
        return NULL;
    }

    split = g_strsplit (g_variant_get_string (string, NULL), "\r\n", -1);
    if (!split) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_INVALID_ARGS,
                     "Invalid GPS NMEA location string: '%s'",
                     g_variant_get_string (string, NULL));
        return NULL;
    }

    /* Create new location object */
    self = mm_location_gps_nmea_new ();

    for (i = 0; split[i]; i++) {
        if (!location_gps_nmea_take_trace (self, split[i]))
            g_free (split[i]);
    }

    /* Note that the strings in the array of strings were already taken
     * or freed */
    g_free (split);

    return self;
}

/*****************************************************************************/

MMLocationGpsNmea *
mm_location_gps_nmea_new (void)
{
    return (MM_LOCATION_GPS_NMEA (
                g_object_new (MM_TYPE_LOCATION_GPS_NMEA, NULL)));
}

static void
mm_location_gps_nmea_init (MMLocationGpsNmea *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_LOCATION_GPS_NMEA,
                                              MMLocationGpsNmeaPrivate);

    self->priv->traces = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                g_free,
                                                g_free);
}

static void
finalize (GObject *object)
{
    MMLocationGpsNmea *self = MM_LOCATION_GPS_NMEA (object);

    g_hash_table_destroy (self->priv->traces);
    if (self->priv->sequence_regex)
        g_regex_unref (self->priv->sequence_regex);

    G_OBJECT_CLASS (mm_location_gps_nmea_parent_class)->finalize (object);
}

static void
mm_location_gps_nmea_class_init (MMLocationGpsNmeaClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMLocationGpsNmeaPrivate));
}
