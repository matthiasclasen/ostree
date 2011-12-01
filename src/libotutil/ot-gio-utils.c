/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include <string.h>

#include "otutil.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

gboolean
ot_gfile_ensure_directory (GFile     *dir,
                           gboolean   with_parents, 
                           GError   **error)
{
  GError *temp_error = NULL;
  gboolean ret = FALSE;

  if (with_parents)
    ret = g_file_make_directory_with_parents (dir, NULL, &temp_error);
  else
    ret = g_file_make_directory (dir, NULL, &temp_error);
  if (!ret)
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      else
        g_clear_error (&temp_error);
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_gfile_load_contents_utf8 (GFile         *file,
                             char         **contents_out,
                             char         **etag_out,
                             GCancellable  *cancellable,
                             GError       **error)
{
  char *ret_contents = NULL;
  char *ret_etag = NULL;
  gsize len;
  gboolean ret = FALSE;

  if (!g_file_load_contents (file, cancellable, &ret_contents, &len, &ret_etag, error))
    goto out;
  if (!g_utf8_validate (ret_contents, len, NULL))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Invalid UTF-8");
      goto out;
    }

  if (contents_out)
    {
      *contents_out = ret_contents;
      ret_contents = NULL;
    }
  if (etag_out)
    {
      *etag_out = ret_etag;
      ret_etag = NULL;
    }
  ret = TRUE;
 out:
  g_free (ret_contents);
  g_free (ret_etag);
  return ret;
}

/* Like g_file_new_for_path, but only do local stuff, not GVFS */
GFile *
ot_gfile_new_for_path (const char *path)
{
  return g_vfs_get_file_for_path (g_vfs_get_local (), path);
}

const char *
ot_gfile_get_path_cached (GFile *file)
{
  const char *path;

  path = g_object_get_data ((GObject*)file, "ostree-file-path");
  if (!path)
    {
      path = g_file_get_path (file);
      g_object_set_data_full ((GObject*)file, "ostree-file-path", (char*)path, (GDestroyNotify)g_free);
    }
  return path;
}


const char *
ot_gfile_get_basename_cached (GFile *file)
{
  const char *name;

  name = g_object_get_data ((GObject*)file, "ostree-file-name");
  if (!name)
    {
      name = g_file_get_basename (file);
      g_object_set_data_full ((GObject*)file, "ostree-file-name", (char*)name, (GDestroyNotify)g_free);
    }
  return name;
}

gboolean
ot_gfile_create_tmp (GFile       *dir,
                     const char  *prefix,
                     const char  *suffix,
                     int          mode,
                     GFile      **out_file,
                     GOutputStream **out_stream,
                     GCancellable *cancellable,
                     GError       **error)
{
  gboolean ret = FALSE;
  GString *tmp_name = NULL;
  int tmpfd = -1;
  GFile *ret_file = NULL;
  GOutputStream *ret_stream = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (!prefix)
    prefix = "tmp-";
  if (!suffix)
    suffix = ".tmp";

  tmp_name = g_string_new (ot_gfile_get_path_cached (dir));
  g_string_append_c (tmp_name, '/');
  g_string_append (tmp_name, prefix);
  g_string_append (tmp_name, "XXXXXX");
  g_string_append (tmp_name, suffix);
  
  tmpfd = g_mkstemp_full (tmp_name->str, O_WRONLY | O_BINARY, mode);
  if (tmpfd == -1)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret_file = ot_gfile_new_for_path (tmp_name->str);
  ret_stream = g_unix_output_stream_new (tmpfd, TRUE);
  
  ret = TRUE;
  if (out_file)
    {
      *out_file = ret_file;
      ret_file = NULL;
    }
  if (out_stream)
    {
      *out_stream = ret_stream;
      ret_stream = NULL;
    }
 out:
  g_clear_object (&ret_file);
  g_clear_object (&ret_stream);
  g_string_free (tmp_name, TRUE);
  return ret;
}

gboolean
ot_gfile_merge_dirs (GFile    *destination,
                     GFile    *src,
                     GCancellable *cancellable,
                     GError   **error)
{
  gboolean ret = FALSE;
  const char *dest_path = NULL;
  const char *src_path = NULL;
  GError *temp_error = NULL;
  GFileInfo *src_fileinfo = NULL;
  GFileInfo *dest_fileinfo = NULL;
  GFileEnumerator *src_enum = NULL;
  GFile *dest_subfile = NULL;
  GFile *src_subfile = NULL;
  const char *name;
  guint32 type;
  const int move_flags = G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA;

  dest_path = ot_gfile_get_path_cached (destination);
  src_path = ot_gfile_get_path_cached (src);

  dest_fileinfo = g_file_query_info (destination, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     cancellable, &temp_error);
  if (dest_fileinfo)
    {
      type = g_file_info_get_attribute_uint32 (dest_fileinfo, "standard::type");
      if (type != G_FILE_TYPE_DIRECTORY)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Attempting to replace non-directory %s with directory %s",
                       dest_path, src_path);
          goto out;
        }

      src_enum = g_file_enumerate_children (src, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);
      if (!src_enum)
        goto out;

      while ((src_fileinfo = g_file_enumerator_next_file (src_enum, cancellable, &temp_error)) != NULL)
        {
          type = g_file_info_get_attribute_uint32 (src_fileinfo, "standard::type");
          name = g_file_info_get_attribute_byte_string (src_fileinfo, "standard::name");
      
          dest_subfile = g_file_get_child (destination, name);
          src_subfile = g_file_get_child (src, name);

          if (type == G_FILE_TYPE_DIRECTORY)
            {
              if (!ot_gfile_merge_dirs (dest_subfile, src_subfile, cancellable, error))
                goto out;
            }
          else
            {
              if (!g_file_move (src_subfile, dest_subfile,
                                move_flags, NULL, NULL, cancellable, error))
                goto out;
            }
          
          g_clear_object (&dest_subfile);
          g_clear_object (&src_subfile);
        }
      if (temp_error)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&temp_error);
      if (!g_file_move (src, destination, move_flags, NULL, NULL, cancellable, error))
        goto out;
    }
  else
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&src_fileinfo);
  g_clear_object (&dest_fileinfo);
  g_clear_object (&src_enum);
  g_clear_object (&dest_subfile);
  g_clear_object (&src_subfile);
  return ret;
}
