/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2016 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include "backends/meta-monitor-config-manager.h"

#include "backends/meta-monitor-manager-private.h"

struct _MetaMonitorConfigManager
{
  GObject parent;

  MetaMonitorManager *monitor_manager;

  MetaMonitorsConfig *current_config;
};

G_DEFINE_TYPE (MetaMonitorConfigManager, meta_monitor_config_manager,
               G_TYPE_OBJECT)

G_DEFINE_TYPE (MetaMonitorsConfig, meta_monitors_config,
               G_TYPE_OBJECT)

MetaMonitorConfigManager *
meta_monitor_config_manager_new (MetaMonitorManager *monitor_manager)
{
  MetaMonitorConfigManager *config_manager;

  config_manager = g_object_new (META_TYPE_MONITOR_CONFIG_MANAGER, NULL);
  config_manager->monitor_manager = monitor_manager;

  return config_manager;
}

static gboolean
is_crtc_assigned (MetaCrtc  *crtc,
                  GPtrArray *crtc_infos)
{
  unsigned int i;

  for (i = 0; i < crtc_infos->len; i++)
    {
      MetaCrtcInfo *assigned_crtc_info = g_ptr_array_index (crtc_infos, i);

      if (assigned_crtc_info->crtc == crtc)
        return TRUE;
    }

  return FALSE;
}

static MetaCrtc *
find_unassigned_crtc (MetaOutput *output,
                      GPtrArray  *crtc_infos)
{
  unsigned int i;

  for (i = 0; i < output->n_possible_crtcs; i++)
    {
      MetaCrtc *crtc = output->possible_crtcs[i];

      if (is_crtc_assigned (crtc, crtc_infos))
        continue;

      return crtc;
    }

  return NULL;
}

typedef struct
{
  MetaLogicalMonitorConfig *logical_monitor_config;
  MetaMonitorConfig *monitor_config;
  GPtrArray *crtc_infos;
  GPtrArray *output_infos;
} MonitorAssignmentData;

static gboolean
assign_monitor_crtc (MetaMonitor         *monitor,
                     MetaMonitorMode     *mode,
                     MetaMonitorCrtcMode *monitor_crtc_mode,
                     gpointer             user_data,
                     GError             **error)
{
  MonitorAssignmentData *data = user_data;
  MetaOutput *output;
  MetaCrtc *crtc;
  MetaCrtcInfo *crtc_info;
  MetaOutputInfo *output_info;
  MetaMonitorConfig *first_monitor_config;
  gboolean assign_output_as_primary;
  gboolean assign_output_as_presentation;

  output = monitor_crtc_mode->output;

  crtc = find_unassigned_crtc (output, data->crtc_infos);
  if (!crtc)
    {
      MetaMonitorSpec *monitor_spec = meta_monitor_get_spec (monitor);

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No available CRTC for monitor '%s %s' not found",
                   monitor_spec->vendor, monitor_spec->product);
      return FALSE;
    }

  crtc_info = g_slice_new0 (MetaCrtcInfo);
  *crtc_info = (MetaCrtcInfo) {
    .crtc = crtc,
    .mode = monitor_crtc_mode->crtc_mode,
    .x = monitor_crtc_mode->x,
    .y = monitor_crtc_mode->y,
    .transform = META_MONITOR_TRANSFORM_NORMAL,
    .outputs = g_ptr_array_new ()
  };
  g_ptr_array_add (crtc_info->outputs, output);

  /*
   * Currently, MetaCrtcInfo are deliberately offset incorrectly to carry over
   * logical monitor location inside the MetaCrtc struct, when in fact this
   * depends on the framebuffer configuration. This will eventually be negated
   * when setting the actual KMS mode.
   *
   * TODO: Remove this hack when we don't need to rely on MetaCrtc to pass
   * logical monitor state.
   */
  crtc_info->x += data->logical_monitor_config->layout.x;
  crtc_info->y += data->logical_monitor_config->layout.y;

  /*
   * Only one output can be marked as primary (due to Xrandr limitation),
   * so only mark the main output of the first monitor in the logical monitor
   * as such.
   */
  first_monitor_config = data->logical_monitor_config->monitor_configs->data;
  if (data->monitor_config == first_monitor_config &&
      meta_monitor_get_main_output (monitor) == output)
    assign_output_as_primary = TRUE;
  else
    assign_output_as_primary = FALSE;

  if (data->logical_monitor_config->is_presentation)
    assign_output_as_presentation = TRUE;
  else
    assign_output_as_presentation = FALSE;

  output_info = g_slice_new0 (MetaOutputInfo);
  *output_info = (MetaOutputInfo) {
    .output = output,
    .is_primary = assign_output_as_primary,
    .is_presentation = assign_output_as_presentation,
    .is_underscanning = output->is_underscanning
  };

  g_ptr_array_add (data->crtc_infos, crtc_info);
  g_ptr_array_add (data->output_infos, output_info);

  return TRUE;
}

static gboolean
assign_monitor_crtcs (MetaMonitorManager       *manager,
                      MetaLogicalMonitorConfig *logical_monitor_config,
                      MetaMonitorConfig        *monitor_config,
                      GPtrArray                *crtc_infos,
                      GPtrArray                *output_infos,
                      GError                  **error)
{
  MetaMonitorSpec *monitor_spec = monitor_config->monitor_spec;
  MetaMonitorModeSpec *monitor_mode_spec = monitor_config->mode_spec;
  MetaMonitor *monitor;
  MetaMonitorMode *monitor_mode;
  MonitorAssignmentData data;

  monitor = meta_monitor_manager_get_monitor_from_spec (manager, monitor_spec);
  if (!monitor)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Configured monitor '%s %s' not found",
                   monitor_spec->vendor, monitor_spec->product);
      return FALSE;
    }

  monitor_mode = meta_monitor_get_mode_from_spec (monitor, monitor_mode_spec);
  if (!monitor_mode)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode %dx%d (%f) for monitor '%s %s'",
                   monitor_mode_spec->width, monitor_mode_spec->height,
                   monitor_mode_spec->refresh_rate,
                   monitor_spec->vendor, monitor_spec->product);
      return FALSE;
    }

  data = (MonitorAssignmentData) {
    .logical_monitor_config = logical_monitor_config,
    .monitor_config = monitor_config,
    .crtc_infos = crtc_infos,
    .output_infos = output_infos
  };
  if (!meta_monitor_mode_foreach_crtc (monitor, monitor_mode,
                                       assign_monitor_crtc,
                                       &data,
                                       error))
    return FALSE;

  return TRUE;
}

static gboolean
assign_logical_monitor_crtcs (MetaMonitorManager       *manager,
                              MetaLogicalMonitorConfig *logical_monitor_config,
                              GPtrArray                *crtc_infos,
                              GPtrArray                *output_infos,
                              GError                  **error)
{
  GList *l;

  for (l = logical_monitor_config->monitor_configs; l; l = l->next)
    {
      MetaMonitorConfig *monitor_config = l->data;

      if (!assign_monitor_crtcs (manager,
                                 logical_monitor_config,
                                 monitor_config,
                                 crtc_infos, output_infos,
                                 error))
        return FALSE;
    }

  return TRUE;
}

gboolean
meta_monitor_config_manager_assign (MetaMonitorManager *manager,
                                    MetaMonitorsConfig *config,
                                    GPtrArray         **out_crtc_infos,
                                    GPtrArray         **out_output_infos,
                                    GError            **error)
{
  GPtrArray *crtc_infos;
  GPtrArray *output_infos;
  GList *l;

  crtc_infos =
    g_ptr_array_new_with_free_func ((GDestroyNotify) meta_crtc_info_free);
  output_infos =
    g_ptr_array_new_with_free_func ((GDestroyNotify) meta_output_info_free);

  for (l = config->logical_monitor_configs; l; l = l->next)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;

      if (!assign_logical_monitor_crtcs (manager, logical_monitor_config,
                                         crtc_infos, output_infos,
                                         error))
        {
          g_ptr_array_free (crtc_infos, TRUE);
          g_ptr_array_free (output_infos, TRUE);
          return FALSE;
        }
    }

  *out_crtc_infos = crtc_infos;
  *out_output_infos = output_infos;

  return TRUE;
}

static MetaMonitor *
find_monitor_with_highest_preferred_resolution (MetaMonitorManager *monitor_manager)
{
  GList *monitors;
  GList *l;
  int largest_area = 0;
  MetaMonitor *largest_monitor = NULL;

  monitors = meta_monitor_manager_get_monitors (monitor_manager);
  for (l = monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;
      MetaMonitorMode *mode;
      int width, height;
      int area;

      mode = meta_monitor_get_preferred_mode (monitor);
      meta_monitor_mode_get_resolution (mode, &width, &height);
      area = width * height;

      if (area > largest_area)
        {
          largest_area = area;
          largest_monitor = monitor;
        }
    }

  return largest_monitor;
}

/*
 * Tries to find the primary monitor as reported by the underlying system;
 * or failing that, a monitor looks to be the laptop panel; or failing that, the
 * monitor with the highest preferred resolution.
 */
static MetaMonitor *
find_primary_monitor (MetaMonitorManager *monitor_manager)
{
  MetaMonitor *monitor;

  monitor = meta_monitor_manager_get_primary_monitor (monitor_manager);
  if (monitor)
    return monitor;

  monitor = meta_monitor_manager_get_laptop_panel (monitor_manager);
  if (monitor)
    return monitor;

  return find_monitor_with_highest_preferred_resolution (monitor_manager);
}

static MetaMonitorConfig *
create_monitor_config (MetaMonitor     *monitor,
                       MetaMonitorMode *mode)
{
  MetaMonitorSpec *monitor_spec;
  MetaMonitorModeSpec *mode_spec;
  MetaMonitorConfig *monitor_config;

  monitor_spec = meta_monitor_get_spec (monitor);
  mode_spec = meta_monitor_mode_get_spec (mode);

  monitor_config = g_new0 (MetaMonitorConfig, 1);
  *monitor_config = (MetaMonitorConfig) {
    .monitor_spec = meta_monitor_spec_clone (monitor_spec),
    .mode_spec = g_memdup (mode_spec, sizeof (MetaMonitorModeSpec))
  };

  return monitor_config;
}

static MetaLogicalMonitorConfig *
create_preferred_logical_monitor_config (MetaMonitor *monitor,
                                         int          x,
                                         int          y)
{
  MetaMonitorMode *mode;
  int width, height;
  MetaMonitorConfig *monitor_config;
  MetaLogicalMonitorConfig *logical_monitor_config;

  mode = meta_monitor_get_preferred_mode (monitor);
  meta_monitor_mode_get_resolution (mode, &width, &height);
  monitor_config = create_monitor_config (monitor, mode);

  logical_monitor_config = g_new0 (MetaLogicalMonitorConfig, 1);
  *logical_monitor_config = (MetaLogicalMonitorConfig) {
    .layout = (MetaRectangle) {
      .x = x,
      .y = y,
      .width = width,
      .height = height
    },
    .monitor_configs = g_list_append (NULL, monitor_config)
  };

  return logical_monitor_config;
}

MetaMonitorsConfig *
meta_monitor_config_manager_create_linear (MetaMonitorConfigManager *config_manager)
{
  MetaMonitorManager *monitor_manager = config_manager->monitor_manager;
  MetaMonitor *primary_monitor;
  MetaMonitorsConfig *config;
  MetaLogicalMonitorConfig *primary_logical_monitor_config;
  int x;
  GList *monitors;
  GList *l;

  primary_monitor = find_primary_monitor (monitor_manager);

  config = g_object_new (META_TYPE_MONITORS_CONFIG, NULL);

  primary_logical_monitor_config =
    create_preferred_logical_monitor_config (primary_monitor, 0, 0);
  primary_logical_monitor_config->is_primary = TRUE;
  config->logical_monitor_configs =
    g_list_append (NULL, primary_logical_monitor_config);

  x = primary_logical_monitor_config->layout.width;
  monitors = meta_monitor_manager_get_monitors (monitor_manager);
  for (l = monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;
      MetaLogicalMonitorConfig *logical_monitor_config;

      if (monitor == primary_monitor)
        continue;

      logical_monitor_config =
        create_preferred_logical_monitor_config (monitor, x, 0);
      config->logical_monitor_configs =
        g_list_append (config->logical_monitor_configs, logical_monitor_config);

      x += logical_monitor_config->layout.width;
    }

  return config;
}

MetaMonitorsConfig *
meta_monitor_config_manager_create_fallback (MetaMonitorConfigManager *config_manager)
{
  MetaMonitorManager *monitor_manager = config_manager->monitor_manager;
  MetaMonitor *primary_monitor;
  MetaMonitorsConfig *config;
  MetaLogicalMonitorConfig *primary_logical_monitor_config;

  primary_monitor = find_primary_monitor (monitor_manager);

  config = g_object_new (META_TYPE_MONITORS_CONFIG, NULL);

  primary_logical_monitor_config =
    create_preferred_logical_monitor_config (primary_monitor, 0, 0);
  primary_logical_monitor_config->is_primary = TRUE;
  config->logical_monitor_configs =
    g_list_append (NULL, primary_logical_monitor_config);

  return config;
}

void
meta_monitor_config_manager_set_current (MetaMonitorConfigManager *config_manager,
                                         MetaMonitorsConfig       *config)
{
  g_set_object (&config_manager->current_config, config);
}

MetaMonitorsConfig *
meta_monitor_config_manager_get_current (MetaMonitorConfigManager *config_manager)
{
  return config_manager->current_config;
}

static void
meta_monitor_config_manager_dispose (GObject *object)
{
  MetaMonitorConfigManager *config_manager =
    META_MONITOR_CONFIG_MANAGER (object);

  g_clear_object (&config_manager->current_config);

  G_OBJECT_CLASS (meta_monitor_config_manager_parent_class)->dispose (object);
}

static void
meta_monitor_config_manager_init (MetaMonitorConfigManager *config_manager)
{
}

static void
meta_monitor_config_manager_class_init (MetaMonitorConfigManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_monitor_config_manager_dispose;
}

static void
meta_monitor_config_free (MetaMonitorConfig *monitor_config)
{
  meta_monitor_spec_free (monitor_config->monitor_spec);
  g_free (monitor_config->mode_spec);
  g_free (monitor_config);
}

static void
meta_logical_monitor_config_free (MetaLogicalMonitorConfig *logical_monitor_config)
{
  g_list_free_full (logical_monitor_config->monitor_configs,
                    (GDestroyNotify) meta_monitor_config_free);
  g_free (logical_monitor_config);
}

static void
meta_monitors_config_finalize (GObject *object)
{
  MetaMonitorsConfig *config = META_MONITORS_CONFIG (object);

  g_list_free_full (config->logical_monitor_configs,
                    (GDestroyNotify) meta_logical_monitor_config_free);
}

static void
meta_monitors_config_init (MetaMonitorsConfig *config)
{
}

static void
meta_monitors_config_class_init (MetaMonitorsConfigClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_monitors_config_finalize;
}