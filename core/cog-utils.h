/*
 * cog-utils.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-export.h"
#include <glib-object.h>

G_BEGIN_DECLS

#if !GLIB_CHECK_VERSION(2, 58, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GEnumClass, g_type_class_unref)
#endif // !GLIB_CHECK_VERSION

COG_API
char* cog_appid_to_dbus_object_path (const char *appid)
    G_GNUC_WARN_UNUSED_RESULT;

COG_API
char* cog_uri_guess_from_user_input (const char *uri_like,
                                     gboolean    is_cli_arg,
                                     GError    **error);

COG_API
GOptionEntry* cog_option_entries_from_class (GObjectClass *klass);


static inline const char*
cog_g_enum_get_nick (GType enum_type, int value)
{
    g_autoptr(GEnumClass) enum_class =
        (GEnumClass_autoptr) g_type_class_ref (enum_type);
    const GEnumValue *enum_value = g_enum_get_value (enum_class, value);
    return enum_value ? enum_value->value_nick : NULL;
}


static inline const GEnumValue*
cog_g_enum_get_value (GType enum_type, const char *nick)
{
    g_autoptr(GEnumClass) enum_class =
        (GEnumClass_autoptr) g_type_class_ref (enum_type);
    return g_enum_get_value_by_nick (enum_class, nick);
}

const gchar*
cog_uri_get_env(const gchar *uri, const gchar *filt);

G_END_DECLS
