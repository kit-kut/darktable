/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/history.h"
#include "common/map_locations.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/range.h"
#include "gui/gtk.h"
#include "gui/preferences_dialogs.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifndef _WIN32
#include <gio/gunixmounts.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <locale.h>

DT_MODULE(1)

#define MAX_RULES 10

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef struct _widgets_sort_t
{
  GtkWidget *box;
  GtkWidget *sort;
  GtkWidget *direction;
} _widgets_sort_t;

typedef struct _widgets_range_t
{
  GtkWidget *range_select;
} _widgets_range_t;

typedef struct _widgets_filename_t
{
  GtkWidget *name;
  GtkWidget *ext;
  GtkWidget *pop;
  GtkWidget *name_tree;
  GtkWidget *ext_tree;
  int internal_change;
} _widgets_filename_t;

typedef struct _widgets_fallback_t
{
  GtkWidget *entry;
} _widgets_fallback_t;

typedef struct dt_lib_filtering_rule_t
{
  int num;

  dt_collection_properties_t prop;

  GtkWidget *w_main;
  GtkWidget *w_operator;
  GtkWidget *w_prop;
  GtkWidget *w_close;
  GtkWidget *w_off;

  GtkWidget *w_widget_box;
  char raw_text[PARAM_STRING_SIZE];
  GtkWidget *w_special_box;
  void *w_specific;      // structure which contains all the widgets specific to the rule type
  int manual_widget_set; // when we update manually the widget, we don't want events to be handled

  gchar *searchstring;
} dt_lib_filtering_rule_t;

typedef struct dt_lib_filtering_t
{
  dt_lib_filtering_rule_t rule[MAX_RULES];
  int nb_rules;

  GtkWidget *rules_box;
  GtkWidget *rules_sw;
  _widgets_sort_t *sort;
  gboolean manual_sort_set;

  gboolean singleclick;
  struct dt_lib_filtering_params_t *params;

  gchar *last_where_ext;
} dt_lib_filtering_t;

typedef struct dt_lib_filtering_params_rule_t
{
  uint32_t item : 16;
  uint32_t mode : 16;
  uint32_t off : 16;
  char string[PARAM_STRING_SIZE];
} dt_lib_filtering_params_rule_t;

typedef struct dt_lib_filtering_params_t
{
  uint32_t rules;
  dt_lib_filtering_params_rule_t rule[MAX_RULES];
} dt_lib_filtering_params_t;

typedef enum _tree_cols_t
{
  TREE_COL_TEXT = 0,
  TREE_COL_TOOLTIP,
  TREE_COL_PATH,
  TREE_COL_COUNT,
  TREE_NUM_COLS
} _tree_cols_t;

static void _filters_gui_update(dt_lib_module_t *self);
static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs, int next,
                                   gpointer self);

const char *name(dt_lib_module_t *self)
{
  return _("filters for collections");
}

void init_presets(dt_lib_module_t *self)
{
  dt_lib_filtering_params_t params;

#define CLEAR_PARAMS(r)                                                                                           \
  {                                                                                                               \
    memset(&params, 0, sizeof(params));                                                                           \
    params.rules = 1;                                                                                             \
    params.rule[0].mode = 0;                                                                                      \
    params.rule[0].item = r;                                                                                      \
  }

  CLEAR_PARAMS(DT_COLLECTION_PROP_RATING);
  g_strlcpy(params.rule[0].string, ">=0", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("rating : all except rejected"), self->plugin_name, self->version(), &params,
                     sizeof(params), TRUE);
  CLEAR_PARAMS(DT_COLLECTION_PROP_RATING);
  g_strlcpy(params.rule[0].string, ">=2", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("rating : ★ ★"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_COLORLABEL);
  g_strlcpy(params.rule[0].string, "red", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("color labels : red"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

#undef CLEAR_PARAMS
}

/* Update the params struct with active ruleset */
static void _filters_update_params(dt_lib_filtering_t *d)
{
  /* reset params */
  dt_lib_filtering_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_filtering_params_t));

  /* for each active rule set update params */
  const int _a = dt_conf_get_int("plugins/lighttable/filtering/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200] = { 0 };
  for(int i = 0; i <= active; i++)
  {
    /* get item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    p->rule[i].item = dt_conf_get_int(confname);

    /* get mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    p->rule[i].mode = dt_conf_get_int(confname);

    /* get on-off */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    p->rule[i].off = dt_conf_get_int(confname);

    /* get string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    const char *string = dt_conf_get_string_const(confname);
    if(string != NULL)
    {
      g_strlcpy(p->rule[i].string, string, PARAM_STRING_SIZE);
    }
  }

  p->rules = active + 1;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  _filters_update_params(self->data);

  /* allocate a copy of params to return, freed by caller */
  *size = sizeof(dt_lib_filtering_params_t);
  void *p = malloc(*size);
  memcpy(p, ((dt_lib_filtering_t *)self->data)->params, *size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  /* update conf settings from params */
  dt_lib_filtering_params_t *p = (dt_lib_filtering_params_t *)params;
  char confname[200] = { 0 };

  gboolean reset_view_filter = FALSE;
  for(uint32_t i = 0; i < p->rules; i++)
  {
    /* set item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1u", i);
    dt_conf_set_int(confname, p->rule[i].item);

    /* set mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1u", i);
    dt_conf_set_int(confname, p->rule[i].mode);

    /* set on-off */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1u", i);
    dt_conf_set_int(confname, p->rule[i].off);

    /* set string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1u", i);
    dt_conf_set_string(confname, p->rule[i].string);

    /* if one of the rules is a rating filter, the view rating filter will be reset to all */
    if(p->rule[i].item == DT_COLLECTION_PROP_RATING)
    {
      reset_view_filter = TRUE;
    }
  }

  if(reset_view_filter)
  {
    dt_view_filter_reset(darktable.view_manager, FALSE);
  }

  /* set number of rules */
  g_strlcpy(confname, "plugins/lighttable/filtering/num_rules", sizeof(confname));
  dt_conf_set_int(confname, p->rules);

  /* update internal params */
  _filters_update_params(self->data);

  /* update ui */
  _filters_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  return 0;
}


const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "lighttable", "map", "print", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static dt_lib_filtering_t *get_collect(dt_lib_filtering_rule_t *r)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)(((char *)r) - r->num * sizeof(dt_lib_filtering_rule_t));
  return d;
}

static void _history_save(dt_lib_filtering_t *d)
{
  // get the string of the rules
  char buf[4096] = { 0 };
  dt_collection_serialize(buf, sizeof(buf), TRUE);

  // compare to last saved history
  char confname[200] = { 0 };
  gchar *str = dt_conf_get_string("plugins/lighttable/filtering/history0");
  if(!g_strcmp0(str, buf))
  {
    g_free(str);
    return;
  }
  g_free(str);

  // remove all subseqeunt history that have the same values
  const int nbmax = dt_conf_get_int("plugins/lighttable/filtering/history_max");
  int move = 0;
  for(int i = 1; i < nbmax; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    gchar *string = dt_conf_get_string(confname);

    if(!g_strcmp0(string, buf))
    {
      move++;
      dt_conf_set_string(confname, "");
    }
    else if(move > 0)
    {
      dt_conf_set_string(confname, "");
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i - move);
      dt_conf_set_string(confname, string);
    }
    g_free(string);
  }

  // move all history entries +1 (and delete the last one)
  for(int i = nbmax - 2; i >= 0; i--)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    gchar *string = dt_conf_get_string(confname);

    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i + 1);
    dt_conf_set_string(confname, string);
    g_free(string);
  }

  // save current history
  dt_conf_set_string("plugins/lighttable/filtering/history0", buf);
}

static void _conf_update_rule(dt_lib_filtering_rule_t *rule)
{
  const dt_lib_collect_mode_t mode = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(rule->w_operator)));
  const gboolean off = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off));

  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", rule->num);
  dt_conf_set_string(confname, rule->raw_text);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", rule->num);
  dt_conf_set_int(confname, rule->prop);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", rule->num);
  dt_conf_set_int(confname, mode);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", rule->num);
  dt_conf_set_int(confname, off);

  _history_save(get_collect(rule));
}

static void _event_rule_changed(GtkWidget *entry, dt_lib_filtering_rule_t *rule)
{
  if(rule->manual_widget_set) return;

  // update the config files
  _conf_update_rule(rule);

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _rule_set_raw_text(dt_lib_filtering_rule_t *rule, const gchar *text, const gboolean signal)
{
  snprintf(rule->raw_text, sizeof(rule->raw_text), "%s", text);
  if(signal) _event_rule_changed(NULL, rule);
}

static gboolean _event_rule_close(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  if(rule->manual_widget_set) return TRUE;

  // decrease the nb of active rules
  dt_lib_filtering_t *d = get_collect(rule);
  if(d->nb_rules <= 0) return FALSE;
  d->nb_rules--;
  dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

  // move up all still active rules by one.
  for(int i = rule->num; i < MAX_RULES - 1; i++)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i + 1);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i + 1);
    const int item = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i + 1);
    const int off = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i + 1);
    gchar *string = dt_conf_get_string(confname);
    if(string)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
      dt_conf_set_int(confname, mode);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
      dt_conf_set_int(confname, item);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
      dt_conf_set_int(confname, off);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
      dt_conf_set_string(confname, string);
      g_free(string);
    }
  }

  _filters_gui_update(self);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  return TRUE;
}

static void _range_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  gchar *txt = dtgtk_range_select_get_raw_text(DTGTK_RANGE_SELECT(special->range_select));
  _rule_set_raw_text(rule, txt, TRUE);
  g_free(txt);
}

static gboolean _range_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;

  rule->manual_widget_set++;
  dtgtk_range_select_set_selection_from_raw_text(DTGTK_RANGE_SELECT(special->range_select), rule->raw_text, FALSE);
  rule->manual_widget_set--;
  return TRUE;
}

static gchar *_rating_print_func(const double value, gboolean detailled)
{
  if(detailled)
  {
    switch((int)floor(value))
    {
      case -1:
        return g_strdup(_("rejected"));
      case 0:
        return g_strdup(_("not rated"));
      case 1:
        return g_strdup("★");
      case 2:
        return g_strdup("★ ★");
      case 3:
        return g_strdup("★ ★ ★");
      case 4:
        return g_strdup("★ ★ ★ ★");
      case 5:
        return g_strdup("★ ★ ★ ★ ★");
    }
  }
  return g_strdup_printf("%.0lf", floor(value));
}

static void _rating_paint_icon(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // first, we set the color depending on the flags
  void *my_data = data;

  if((flags & CPF_PRELIGHT) || (flags & CPF_ACTIVE))
  {
    // we want a filled icon
    GdkRGBA bc = darktable.gui->colors[DT_GUI_COLOR_RANGE_ICONS];
    GdkRGBA *shade_color = gdk_rgba_copy(&bc);
    shade_color->alpha *= 0.6;
    my_data = shade_color;
  }

  // then we draw the regular icon
  dtgtk_cairo_paint_star(cr, x, y, w, h, flags, my_data);
}
static void _rating_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), FALSE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  range->step_bd = 1.0;
  dtgtk_range_select_add_icon(range, 7, -1, dtgtk_cairo_paint_reject, 0, NULL);
  dtgtk_range_select_add_icon(range, 22, 0, dtgtk_cairo_paint_unratestar, 0, NULL);
  dtgtk_range_select_add_icon(range, 36, 1, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 50, 2, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 64, 3, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 78, 4, _rating_paint_icon, 0, NULL);
  dtgtk_range_select_add_icon(range, 93, 5, _rating_paint_icon, 0, NULL);
  range->print = _rating_print_func;

  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT CASE WHEN (flags & 8) == 8 THEN -1 ELSE (flags & 7) END AS rating,"
             " COUNT(*) AS count"
             " FROM main.images AS mi"
             " GROUP BY rating"
             " ORDER BY rating");
  int nb[7] = { 0 };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int val = sqlite3_column_int(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    if(val < 6 && val >= -1) nb[val + 1] += count;
  }
  sqlite3_finalize(stmt);

  dtgtk_range_select_add_range_block(range, 1.0, 1.0, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX, _("all images"),
                                     nb[0] + nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
  dtgtk_range_select_add_range_block(range, 0.0, 1.0, DT_RANGE_BOUND_MAX, _("all except rejected"),
                                     nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6]);
  dtgtk_range_select_add_range_block(range, -1.0, -1.0, DT_RANGE_BOUND_FIXED, _("rejected only"), nb[0]);
  dtgtk_range_select_add_range_block(range, 0.0, 0.0, DT_RANGE_BOUND_FIXED, _("unstared only"), nb[1]);
  dtgtk_range_select_add_range_block(range, 1.0, 5.0, DT_RANGE_BOUND_MAX, "★", nb[2]);
  dtgtk_range_select_add_range_block(range, 2.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★", nb[3]);
  dtgtk_range_select_add_range_block(range, 3.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★", nb[4]);
  dtgtk_range_select_add_range_block(range, 4.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★", nb[5]);
  dtgtk_range_select_add_range_block(range, 5.0, 5.0, DT_RANGE_BOUND_MAX, "★ ★ ★ ★ ★", nb[6]);

  range->min_r = -1;
  range->max_r = 5.999;
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static gboolean _ratio_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = get_collect(rule);
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT ROUND(aspect_ratio,3), COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY ROUND(aspect_ratio,3)",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int nb_portrait = 0;
  int nb_square = 0;
  int nb_landscape = 0;
  dtgtk_range_select_reset_blocks(range);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    if(val < 1.0)
      nb_portrait += count;
    else if(val > 1.0)
      nb_landscape += count;
    else
      nb_square += count;

    dtgtk_range_select_add_block(range, val, count);
  }
  sqlite3_finalize(stmt);

  // predefined selections
  dtgtk_range_select_add_range_block(range, 1.0, 1.0, DT_RANGE_BOUND_MIN | DT_RANGE_BOUND_MAX, _("all images"),
                                     nb_portrait + nb_square + nb_landscape);
  dtgtk_range_select_add_range_block(range, 0.5, 0.99, DT_RANGE_BOUND_MIN, _("portrait images"), nb_portrait);
  dtgtk_range_select_add_range_block(range, 1.0, 1.0, DT_RANGE_BOUND_FIXED, _("square images"), nb_square);
  dtgtk_range_select_add_range_block(range, 1.01, 2.0, DT_RANGE_BOUND_MAX, _("landsacpe images"), nb_landscape);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  return TRUE;
}

static double _ratio_value_to_band_func(const double value)
{
  if(value >= 1.0) return value;
  // for value < 1 (portrait), we want the inverse of the value
  return 2.0 - 1.0 / value;
}

static double _ratio_value_from_band_func(const double value)
{
  if(value >= 1.0) return value;
  // for value < 1 (portrait), we want the inverse of the value
  return 1.0 / (2.0 - value);
}

static gchar *_ratio_print_func(const double value, gboolean detailled)
{
  gchar *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_NUMERIC, "C");
  gchar *txt = g_strdup_printf("%.2lf", value);
  setlocale(LC_NUMERIC, locale);
  g_free(locale);

  if(detailled)
  {
    if(value < 1.0)
      return dt_util_dstrcat(txt, " %s", _("portrait"));
    else if(value > 1.0)
      return dt_util_dstrcat(txt, " %s", _("landscape"));
    else if(value == 1.0)
      return dt_util_dstrcat(txt, " %s", _("square"));
  }
  return txt;
}

static void _ratio_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                               const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  dtgtk_range_select_set_band_func(range, _ratio_value_from_band_func, _ratio_value_to_band_func);
  dtgtk_range_select_add_marker(range, 1.0, TRUE);
  range->print = _ratio_print_func;

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT MIN(aspect_ratio), MAX(aspect_ratio)"
             " FROM main.images");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  double min = 0.0;
  double max = 4.0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = sqlite3_column_double(stmt, 0);
    max = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);
  range->min_r = min;
  range->max_r = max;

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static gboolean _focal_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = get_collect(rule);
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT ROUND(focal_length,0), COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY ROUND(focal_length,0)",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    dtgtk_range_select_add_block(range, val, count);
  }
  sqlite3_finalize(stmt);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  return TRUE;
}

static gchar *_focal_print_func(const double value, gboolean detailled)
{
  gchar *txt = g_strdup_printf("%.0lf", value);

  if(detailled)
  {
    dt_util_dstrcat(txt, " %s", _("mm."));
  }
  return txt;
}

static void _focal_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                               const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  range->step_bd = 1.0;
  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  range->print = _focal_print_func;

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT MIN(focal_length), MAX(focal_length)"
             " FROM main.images");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  double min = 0.0;
  double max = 400.0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = sqlite3_column_double(stmt, 0);
    max = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);
  range->min_r = floor(min);
  range->max_r = floor(max) + 1.0;

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static gboolean _aperture_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = get_collect(rule);
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT ROUND(aperture,1), COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY ROUND(aperture,1)",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    dtgtk_range_select_add_block(range, val, count);
  }
  sqlite3_finalize(stmt);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  return TRUE;
}

static gchar *_aperture_print_func(const double value, gboolean detailled)
{
  gchar *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_NUMERIC, "C");
  gchar *txt = g_strdup_printf("%s%.1lf", detailled ? "f/" : "", value);
  setlocale(LC_NUMERIC, locale);
  g_free(locale);

  return txt;
}

static void _aperture_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);
  range->step_bd = 1.0;
  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  range->print = _aperture_print_func;

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT MIN(aperture), MAX(aperture)"
             " FROM main.images");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  double min = 0.0;
  double max = 22.0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = sqlite3_column_double(stmt, 0);
    max = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);
  range->min_r = floor(min * 10.0) / 10.0;
  range->max_r = (floor(max * 10.0) + 1.0) / 10.0;

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static gboolean _iso_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = get_collect(rule);
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT ROUND(iso,0), COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY ROUND(iso, 0)",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    dtgtk_range_select_add_block(range, val, count);
  }
  sqlite3_finalize(stmt);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  return TRUE;
}

static double _iso_value_to_band_func(const double value)
{
  if(value <= 1) return 0; // this shouldn't happen
  return log2(value / 100.0);
}

static double _iso_value_from_band_func(const double value)
{
  return 100 * pow(2.0, value);
}

static gchar *_iso_print_func(const double value, gboolean detailled)
{
  if(detailled)
  {
    // we round the value
    double v = value;
    if(value < 200)
      v = round(v / 25) * 25;
    else
      v = round(v / 50) * 50;
    return g_strdup_printf("%.0lf ISO", v);
  }
  else
  {
    gchar *locale = strdup(setlocale(LC_ALL, NULL));
    setlocale(LC_NUMERIC, "C");
    gchar *txt = g_strdup_printf("%.0lf", value);
    setlocale(LC_NUMERIC, locale);
    g_free(locale);
    return txt;
  }
}

static void _iso_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                             const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  dtgtk_range_select_set_band_func(range, _iso_value_from_band_func, _iso_value_to_band_func);
  range->print = _iso_print_func;

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT MIN(iso), MAX(iso)"
             " FROM main.images");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  double min = 50;
  double max = 12800;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = sqlite3_column_double(stmt, 0);
    max = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);
  range->min_r = floor(min);
  range->max_r = floor(max) + 1;

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static gboolean _exposure_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = get_collect(rule);
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT exposure, COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY exposure",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);

    dtgtk_range_select_add_block(range, val, count);
  }
  sqlite3_finalize(stmt);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  return TRUE;
}

static double _exposure_value_to_band_func(const double value)
{
  return pow(value, 0.25);
}

static double _exposure_value_from_band_func(const double value)
{
  return pow(value, 4);
}

static gchar *_exposure_print_func(const double value, gboolean detailled)
{
  if(detailled)
  {
    return dt_util_format_exposure(value);
  }
  else
  {
    gchar *locale = strdup(setlocale(LC_ALL, NULL));
    setlocale(LC_NUMERIC, "C");
    gchar *txt = g_strdup_printf("%.6lf", value);
    setlocale(LC_NUMERIC, locale);
    g_free(locale);
    return txt;
  }
}

static void _exposure_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE, DT_RANGE_TYPE_NUMERIC);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  gtk_entry_set_width_chars(GTK_ENTRY(range->entry_min), 10);
  gtk_entry_set_width_chars(GTK_ENTRY(range->entry_max), 10);
  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);
  dtgtk_range_select_set_band_func(range, _exposure_value_from_band_func, _exposure_value_to_band_func);
  dtgtk_range_select_add_marker(range, 1.0, TRUE);
  range->print = _exposure_print_func;

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT MIN(exposure), MAX(exposure)"
             " FROM main.images");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  double min = 0.0;
  double max = 2.0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = sqlite3_column_double(stmt, 0);
    max = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);
  range->min_r = min;
  range->max_r = max;

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static gboolean _date_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  dt_lib_filtering_t *d = get_collect(rule);
  _widgets_range_t *special = (_widgets_range_t *)rule->w_specific;
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  rule->manual_widget_set++;
  // first, we update the graph
  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT SUBSTR(datetime_taken, 1, 19) AS date, COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE datetime_taken IS NOT NULL AND LENGTH(datetime_taken)>=19 AND %s"
             " GROUP BY date",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  dtgtk_range_select_reset_blocks(range);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int count = sqlite3_column_int(stmt, 1);

    GDateTime *dt = dt_datetime_exif_to_gdatetime((const char *)sqlite3_column_text(stmt, 0), darktable.utc_tz);
    if(dt)
    {
      dtgtk_range_select_add_block(range, g_date_time_to_unix(dt), count);
      g_date_time_unref(dt);
    }
  }
  sqlite3_finalize(stmt);

  // and setup the selection
  dtgtk_range_select_set_selection_from_raw_text(range, rule->raw_text, FALSE);
  rule->manual_widget_set--;

  dtgtk_range_select_redraw(range);
  return TRUE;
}

static void _date_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                              const gchar *text, dt_lib_module_t *self)
{
  _widgets_range_t *special = (_widgets_range_t *)g_malloc0(sizeof(_widgets_range_t));

  special->range_select
      = dtgtk_range_select_new(dt_collection_name_untranslated(prop), TRUE, DT_RANGE_TYPE_DATETIME);
  GtkDarktableRangeSelect *range = DTGTK_RANGE_SELECT(special->range_select);

  range->type = DT_RANGE_TYPE_DATETIME;
  range->step_bd = 86400; // step of 1 day (in seconds)
  dtgtk_range_select_set_selection_from_raw_text(range, text, FALSE);

  char query[1024] = { 0 };
  g_snprintf(query, sizeof(query),
             "SELECT SUBSTR(MIN(datetime_taken),1,19), SUBSTR(MAX(datetime_taken),1,19)"
             " FROM main.images"
             " WHERE datetime_taken IS NOT NULL AND LENGTH(datetime_taken)>=19");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  gchar *min = NULL;
  gchar *max = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    max = g_strdup((const char *)sqlite3_column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);
  if(min && max)
  {
    GDateTime *dtmin = dt_datetime_exif_to_gdatetime(min, darktable.utc_tz);
    if(dtmin)
    {
      range->min_r = g_date_time_to_unix(dtmin);
      g_date_time_unref(dtmin);
    }
    g_free(min);
    GDateTime *dtmax = dt_datetime_exif_to_gdatetime(max, darktable.utc_tz);
    if(dtmax)
    {
      range->max_r = g_date_time_to_unix(dtmax);
      g_date_time_unref(dtmax);
    }
    g_free(max);
  }

  gtk_box_pack_start(GTK_BOX(rule->w_special_box), special->range_select, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), rule);

  rule->w_specific = special;
}

static void _filename_decode(const gchar *txt, gchar **name, gchar **ext)
{
  if(!txt || strlen(txt) == 0) return;

  // split the path to find filename and extension parts
  gchar **elems = g_strsplit(txt, "/", -1);
  const unsigned int size = g_strv_length(elems);
  if(size == 2)
  {
    *name = g_strdup(elems[0]);
    *ext = g_strdup(elems[1]);
  }
  g_strfreev(elems);
}

static void _filename_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  gchar *value = g_strdup_printf("%s/%s", gtk_entry_get_text(GTK_ENTRY(filename->name)),
                                 gtk_entry_get_text(GTK_ENTRY(filename->ext)));

  _rule_set_raw_text(rule, value, TRUE);
  g_free(value);
}

void _filename_tree_update_visibility(GtkWidget *w, dt_lib_filtering_rule_t *rule)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;

  gtk_widget_set_visible(gtk_widget_get_parent(filename->name_tree), w == filename->name);
  gtk_widget_set_visible(gtk_widget_get_parent(filename->ext_tree), w == filename->ext);
}

void _filename_tree_update(dt_lib_filtering_rule_t *rule)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
  dt_lib_filtering_t *d = get_collect(rule);

  char query[1024] = { 0 };
  int nb_raw = 0;
  int nb_not_raw = 0;
  int nb_ldr = 0;
  int nb_hdr = 0;

  GtkTreeIter iter;
  GtkTreeModel *name_model = gtk_tree_view_get_model(GTK_TREE_VIEW(filename->name_tree));
  gtk_list_store_clear(GTK_LIST_STORE(name_model));
  GtkTreeModel *ext_model = gtk_tree_view_get_model(GTK_TREE_VIEW(filename->ext_tree));
  gtk_list_store_clear(GTK_LIST_STORE(ext_model));
  g_snprintf(query, sizeof(query),
             "SELECT filename, COUNT(*) AS count, flags"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY filename"
             " ORDER BY filename",
             d->last_where_ext);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *fn = (const char *)sqlite3_column_text(stmt, 0);
    if(fn == NULL) continue; // safeguard against degenerated db entries
    const int count = sqlite3_column_int(stmt, 1);
    const int flags = sqlite3_column_int(stmt, 2);

    gchar *ext = g_strrstr(fn, ".");
    char name[1024] = { 0 };
    g_snprintf(name, MIN(strlen(fn) - strlen(ext) + 1, sizeof(name)), "%s", fn);

    // we search throught the tree to find an already existing name like this
    gboolean found = FALSE;
    gboolean iterok = gtk_tree_model_get_iter_first(name_model, &iter);
    while(iterok)
    {
      // if it's the same as the value, then increment count and exit
      gchar *text = NULL;
      gtk_tree_model_get(name_model, &iter, TREE_COL_PATH, &text, -1);
      if(!g_strcmp0(text, name))
      {
        int nb = 0;
        gtk_tree_model_get(name_model, &iter, TREE_COL_COUNT, &nb, -1);
        nb += MAX(count, 1);
        gtk_list_store_set(GTK_LIST_STORE(name_model), &iter, TREE_COL_COUNT, nb, -1);
        found = TRUE;
        break;
      }

      // test next iter
      iterok = gtk_tree_model_iter_next(name_model, &iter);
    }
    if(!found)
    {
      // create a new iter
      gtk_list_store_append(GTK_LIST_STORE(name_model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(name_model), &iter, TREE_COL_TEXT, name, TREE_COL_TOOLTIP, name,
                         TREE_COL_PATH, name, TREE_COL_COUNT, count, -1);
    }

    // and we do the same for extensions
    found = FALSE;
    iterok = gtk_tree_model_get_iter_first(ext_model, &iter);
    while(iterok)
    {
      // if it's the same as the value, then increment count and exit
      gchar *text = NULL;
      gtk_tree_model_get(ext_model, &iter, TREE_COL_PATH, &text, -1);
      if(!g_strcmp0(text, ext))
      {
        int nb = 0;
        gtk_tree_model_get(ext_model, &iter, TREE_COL_COUNT, &nb, -1);
        nb += MAX(count, 1);
        gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_COUNT, nb, -1);
        if(flags & DT_IMAGE_RAW)
          nb_raw += count;
        else
          nb_not_raw += count;
        if(flags & DT_IMAGE_LDR) nb_ldr += count;
        if(flags & DT_IMAGE_HDR) nb_hdr += count;
        found = TRUE;
        break;
      }

      // test next iter
      iterok = gtk_tree_model_iter_next(ext_model, &iter);
    }
    if(!found)
    {
      // create a new iter
      gtk_list_store_append(GTK_LIST_STORE(ext_model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, ext, TREE_COL_TOOLTIP, ext,
                         TREE_COL_PATH, ext, TREE_COL_COUNT, count, -1);

      if(flags & DT_IMAGE_RAW)
        nb_raw += count;
      else
        nb_not_raw += count;
      if(flags & DT_IMAGE_LDR) nb_ldr += count;
      if(flags & DT_IMAGE_HDR) nb_hdr += count;
    }
  }
  sqlite3_finalize(stmt);

  // and we insert the predefined extensions
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "", TREE_COL_TOOLTIP, "", TREE_COL_PATH, "",
                     TREE_COL_COUNT, 0, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "HDR", TREE_COL_TOOLTIP,
                     "hight dynamic range files", TREE_COL_PATH, "HDR", TREE_COL_COUNT, nb_hdr, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "LDR", TREE_COL_TOOLTIP,
                     "low dynamic range files", TREE_COL_PATH, "LDR", TREE_COL_COUNT, nb_ldr, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "NOT RAW", TREE_COL_TOOLTIP,
                     "all expect RAW files", TREE_COL_PATH, "NOT RAW", TREE_COL_COUNT, nb_not_raw, -1);
  gtk_list_store_insert(GTK_LIST_STORE(ext_model), &iter, 0);
  gtk_list_store_set(GTK_LIST_STORE(ext_model), &iter, TREE_COL_TEXT, "RAW", TREE_COL_TOOLTIP, "RAW files",
                     TREE_COL_PATH, "RAW", TREE_COL_COUNT, nb_raw, -1);
}

static gboolean _filename_select_func(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  GtkTreeSelection *sel = (GtkTreeSelection *)data;
  gchar **elems = (gchar **)g_object_get_data(G_OBJECT(sel), "elems");
  gchar *str = NULL;
  gtk_tree_model_get(model, iter, TREE_COL_PATH, &str, -1);

  for(int i = 0; i < g_strv_length(elems); i++)
  {
    if(!g_strcmp0(str, elems[i]))
    {
      gtk_tree_selection_select_path(sel, path);
      break;
    }
  }

  return FALSE;
}

static void _filename_update_selection(dt_lib_filtering_rule_t *rule)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;

  // get the current text
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(filename->pop));
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(entry));

  // get the current treeview
  GtkWidget *tree = (entry == filename->name) ? filename->name_tree : filename->ext_tree;

  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  filename->internal_change++;
  gtk_tree_selection_unselect_all(sel);

  if(g_strcmp0(txt, ""))
  {
    gchar **elems = g_strsplit(txt, ",", -1);
    g_object_set_data(G_OBJECT(sel), "elems", elems);
    gtk_tree_model_foreach(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), _filename_select_func, sel);
    g_strfreev(elems);
  }
  filename->internal_change--;
}

static gboolean _filename_press(GtkWidget *w, GdkEventButton *e, dt_lib_filtering_rule_t *rule)
{
  if(e->button == 3)
  {
    _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
    _filename_tree_update_visibility(w, rule);
    gtk_popover_set_default_widget(GTK_POPOVER(filename->pop), w);
    gtk_popover_set_relative_to(GTK_POPOVER(filename->pop), w);

    // update the selection
    _filename_update_selection(rule);

    gtk_widget_show_all(filename->pop);
    return TRUE;
  }
  return FALSE;
}

static gboolean _filename_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  gchar *name = NULL;
  gchar *ext = NULL;
  _filename_decode(rule->raw_text, &name, &ext);
  _filename_tree_update(rule);

  rule->manual_widget_set++;
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
  if(name) gtk_entry_set_text(GTK_ENTRY(filename->name), name);
  if(ext) gtk_entry_set_text(GTK_ENTRY(filename->ext), ext);
  rule->manual_widget_set--;

  g_free(name);
  g_free(ext);
  return TRUE;
}

static void _filename_popup_closed(GtkWidget *w, dt_lib_filtering_rule_t *rule)
{
  // we validate the corresponding entry
  gtk_widget_activate(gtk_popover_get_default_widget(GTK_POPOVER(w)));
}

static void _filename_tree_row_activated(GtkTreeView *self, GtkTreePath *path, GtkTreeViewColumn *column,
                                         dt_lib_filtering_rule_t *rule)
{
  // as the selection as already been updated on first click, we just close the popup
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
  gtk_widget_hide(filename->pop);
}

static void _filename_tree_selection_change(GtkTreeSelection *sel, dt_lib_filtering_rule_t *rule)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
  if(!filename || filename->internal_change) return;
  GtkTreeModel *model = gtk_tree_view_get_model(gtk_tree_selection_get_tree_view(sel));
  GList *list = gtk_tree_selection_get_selected_rows(sel, NULL);

  gchar *txt = NULL;
  for(const GList *l = list; l; l = g_list_next(l))
  {
    GtkTreePath *path = (GtkTreePath *)l->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, path))
    {
      gchar *val = NULL;
      gtk_tree_model_get(model, &iter, TREE_COL_PATH, &val, -1);
      if(val) txt = dt_util_dstrcat(txt, "%s%s", (txt == NULL) ? "" : ",", val);
    }
  }
  g_list_free_full(list, (GDestroyNotify)gtk_tree_path_free);

  // we set the entry with this value
  GtkWidget *entry = gtk_popover_get_default_widget(GTK_POPOVER(filename->pop));
  gtk_entry_set_text(GTK_ENTRY(entry), (txt == NULL) ? "" : txt);
  g_free(txt);
}

static void _filename_ok_clicked(GtkWidget *w, dt_lib_filtering_rule_t *rule)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)rule->w_specific;
  gtk_widget_hide(filename->pop);
}

void _filename_tree_count_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                               GtkTreeIter *iter, gpointer data)
{
  gchar *name;
  guint count;

  gtk_tree_model_get(model, iter, TREE_COL_TEXT, &name, TREE_COL_COUNT, &count, -1);
  if(!g_strcmp0(name, "") && count == 0)
  {
    g_object_set(renderer, "text", name, NULL);
    g_object_set(renderer, "sensitive", FALSE, NULL);
  }
  else
  {
    gchar *coltext = g_strdup_printf("%s (%d)", name, count);
    g_object_set(renderer, "text", coltext, NULL);
    g_free(coltext);
    g_object_set(renderer, "sensitive", TRUE, NULL);
  }

  g_free(name);
}

static void _filename_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self)
{
  _widgets_filename_t *filename = (_widgets_filename_t *)g_malloc0(sizeof(_widgets_filename_t));

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), hb, TRUE, TRUE, 0);
  filename->name = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(filename->name), 0);
  gtk_widget_set_can_default(filename->name, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(filename->name), _("filename"));
  gtk_widget_set_tooltip_text(filename->name, _("enter filename to search.\n"
                                                "multiple value can be separated by ','\n"
                                                "\nright-click to get existing filenames."));
  gtk_box_pack_start(GTK_BOX(hb), filename->name, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(filename->name), "activate", G_CALLBACK(_filename_changed), rule);
  g_signal_connect(G_OBJECT(filename->name), "button-press-event", G_CALLBACK(_filename_press), rule);

  filename->ext = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(filename->ext), 0);
  gtk_widget_set_can_default(filename->ext, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(filename->ext), _("extension"));
  gtk_widget_set_tooltip_text(filename->ext, _("enter extension to search with starting dot.\n"
                                               "multiple value can be separated by ','\n"
                                               "handled keyword : 'RAW' 'NOT RAW' 'LDR' 'HDR'\n"
                                               "\nright-click to get existing extensions."));
  gtk_box_pack_start(GTK_BOX(hb), filename->ext, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(filename->ext), "activate", G_CALLBACK(_filename_changed), rule);
  g_signal_connect(G_OBJECT(filename->ext), "button-press-event", G_CALLBACK(_filename_press), rule);

  // the popup
  filename->pop = gtk_popover_new(filename->name);
  gtk_widget_set_size_request(filename->pop, 250, 400);
  g_signal_connect(G_OBJECT(filename->pop), "closed", G_CALLBACK(_filename_popup_closed), rule);
  hb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(filename->pop), hb);

  // the name tree
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_no_show_all(sw, TRUE);
  gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
  GtkTreeModel *model
      = GTK_TREE_MODEL(gtk_list_store_new(TREE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT));
  filename->name_tree = gtk_tree_view_new_with_model(model);
  gtk_widget_show(filename->name_tree);
  gtk_widget_set_tooltip_text(filename->name_tree, _("simple click to select filename\n"
                                                     "ctrl-click to select multiple values"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(filename->name_tree), FALSE);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(filename->name_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(filename->name_tree), "row-activated", G_CALLBACK(_filename_tree_row_activated), rule);
  g_signal_connect(G_OBJECT(sel), "changed", G_CALLBACK(_filename_tree_selection_change), rule);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(filename->name_tree), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _filename_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(filename->name_tree), TREE_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), filename->name_tree);

  // the extension tree
  sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_no_show_all(sw, TRUE);
  gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
  model
      = GTK_TREE_MODEL(gtk_list_store_new(TREE_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT));
  filename->ext_tree = gtk_tree_view_new_with_model(model);
  gtk_widget_show(filename->ext_tree);
  gtk_widget_set_tooltip_text(filename->ext_tree, _("simple click to select extension\n"
                                                    "ctrl-click to select multiple values"));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(filename->ext_tree), FALSE);
  sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(filename->ext_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(filename->name_tree), "row-activated", G_CALLBACK(_filename_tree_row_activated), rule);
  g_signal_connect(G_OBJECT(sel), "changed", G_CALLBACK(_filename_tree_selection_change), rule);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(filename->ext_tree), col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _filename_tree_count_func, NULL, NULL);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(filename->ext_tree), TREE_COL_TOOLTIP);

  gtk_container_add(GTK_CONTAINER(sw), filename->ext_tree);

  // the button to close the popup
  GtkWidget *btn = gtk_button_new_with_label(_("ok"));
  gtk_box_pack_start(GTK_BOX(hb), btn, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_filename_ok_clicked), rule);

  rule->w_specific = filename;
}

static void _fallback_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_fallback_t *fallback = (_widgets_fallback_t *)rule->w_specific;

  _rule_set_raw_text(rule, gtk_entry_get_text(GTK_ENTRY(fallback->entry)), TRUE);
}

static gboolean _fallback_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;

  rule->manual_widget_set++;
  _widgets_fallback_t *fallback = (_widgets_fallback_t *)rule->w_specific;
  gtk_entry_set_text(GTK_ENTRY(fallback->entry), rule->raw_text);
  rule->manual_widget_set--;

  return TRUE;
}

static void _fallback_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self)
{
  _widgets_fallback_t *fallback = (_widgets_fallback_t *)g_malloc0(sizeof(_widgets_fallback_t));

  fallback->entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(fallback->entry), text);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), fallback->entry, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(fallback->entry), "activate", G_CALLBACK(_fallback_changed), rule);

  rule->w_specific = fallback;
}

static gboolean _widget_update(dt_lib_filtering_rule_t *rule)
{
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_RATING:
      return _range_update(rule);
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      return _ratio_update(rule);
    case DT_COLLECTION_PROP_FOCAL_LENGTH:
      return _focal_update(rule);
    case DT_COLLECTION_PROP_APERTURE:
      return _aperture_update(rule);
    case DT_COLLECTION_PROP_ISO:
      return _iso_update(rule);
    case DT_COLLECTION_PROP_EXPOSURE:
      return _exposure_update(rule);
    case DT_COLLECTION_PROP_TIME:
      return _date_update(rule);
    case DT_COLLECTION_PROP_FILENAME:
      return _filename_update(rule);
    default:
      return _fallback_update(rule);
  }
}

static gboolean _widget_init_special(dt_lib_filtering_rule_t *rule, const gchar *text, dt_lib_module_t *self)
{
  // if the widgets already exits, destroy them
  if(rule->w_special_box)
  {
    gtk_widget_destroy(rule->w_special_box);
    rule->w_special_box = NULL;
    g_free(rule->w_specific);
    rule->w_specific = NULL;
  }

  // recreate the box
  rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(rule->w_special_box, "collect-rule-special");
  gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_special_box, TRUE, TRUE, 0);

  // initialize the specific entries if any
  gboolean widgets_ok = FALSE;
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_RATING:
      _rating_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      _ratio_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_FOCAL_LENGTH:
      _focal_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_APERTURE:
      _aperture_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_ISO:
      _iso_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_EXPOSURE:
      _exposure_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_TIME:
      _date_widget_init(rule, rule->prop, text, self);
      break;
    case DT_COLLECTION_PROP_FILENAME:
      _filename_widget_init(rule, rule->prop, text, self);
      break;
    default:
      _fallback_widget_init(rule, rule->prop, text, self);
      break;
  }

  widgets_ok = _widget_update(rule);

  // set the visibility for the eventual special widgets
  if(rule->w_specific)
  {
    gtk_widget_show_all(rule->w_special_box); // we ensure all the childs widgets are shown by default
    gtk_widget_set_no_show_all(rule->w_special_box, TRUE);

    // special/raw state is stored per rule property
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/raw_%d", rule->prop);
    const gboolean special = (widgets_ok && !dt_conf_get_bool(confname));

    gtk_widget_set_visible(rule->w_special_box, special);
  }
  else
  {
    gtk_widget_set_no_show_all(rule->w_special_box, TRUE);
    gtk_widget_set_visible(rule->w_special_box, FALSE);
  }


  return (rule->w_specific != NULL);
}

static void _event_rule_change_type(GtkWidget *widget, dt_lib_module_t *self)
{
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "collect_data");
  if(mode == rule->prop) return;

  const dt_collection_properties_t oldprop = rule->prop;
  rule->prop = mode;
  gtk_button_set_label(GTK_BUTTON(rule->w_prop), dt_collection_name(mode));

  // increase the nb of use of this property
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/nb_use_%d", mode);
  dt_conf_set_int(confname, dt_conf_get_int(confname) + 1);

  // re-init the special widgets
  _widget_init_special(rule, "", self);

  // reset the raw entry
  _rule_set_raw_text(rule, "", FALSE);

  // update the config files
  _conf_update_rule(rule);

  // when tag was/become the first rule, we need to handle the order
  if(rule->num == 0)
  {
    gboolean order_request = FALSE;
    uint32_t order = 0;
    if(oldprop != DT_COLLECTION_PROP_TAG && rule->prop == DT_COLLECTION_PROP_TAG)
    {
      // save global order
      const uint32_t sort = dt_collection_get_sort_field(darktable.collection);
      const gboolean descending = dt_collection_get_sort_descending(darktable.collection);
      dt_conf_set_int("plugins/lighttable/filtering/order", sort | (descending ? DT_COLLECTION_ORDER_FLAG : 0));
    }
    else if(oldprop == DT_COLLECTION_PROP_TAG && rule->prop != DT_COLLECTION_PROP_TAG)
    {
      // restore global order
      order = dt_conf_get_int("plugins/lighttable/filtering/order");
      order_request = TRUE;
      dt_collection_set_tag_id((dt_collection_t *)darktable.collection, 0);
    }
    if(order_request) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE, order);
  }

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _event_append_rule(GtkWidget *widget, dt_lib_module_t *self)
{
  // add new rule
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  char confname[200] = { 0 };

  if(mode >= 0)
  {
    // add an empty rule
    if(d->nb_rules >= MAX_RULES)
    {
      dt_control_log("You can't have more than %d rules", MAX_RULES);
      return;
    }
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", d->nb_rules);
    dt_conf_set_int(confname, mode);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", d->nb_rules);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", d->nb_rules);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", d->nb_rules);
    dt_conf_set_string(confname, "");
    d->nb_rules++;
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/nb_use_%d", mode);
    dt_conf_set_int(confname, dt_conf_get_int(confname) + 1);

    _filters_gui_update(self);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF,
                               NULL);
  }
}

static void _popup_add_item(GtkMenuShell *pop, const gchar *name, const int id, const gboolean title,
                            GCallback callback, gpointer data, dt_lib_module_t *self)
{
  GtkWidget *smt = gtk_menu_item_new_with_label(name);
  if(title)
  {
    gtk_widget_set_name(smt, "collect-popup-title");
    gtk_widget_set_sensitive(smt, FALSE);
  }
  else
  {
    gtk_widget_set_name(smt, "collect-popup-item");
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
    gtk_label_set_xalign(GTK_LABEL(child), 1.0);
    g_object_set_data(G_OBJECT(smt), "collect_id", GINT_TO_POINTER(id));
    if(data) g_object_set_data(G_OBJECT(smt), "collect_data", data);
    g_signal_connect(G_OBJECT(smt), "activate", callback, self);
  }
  gtk_menu_shell_append(pop, smt);
}

static gboolean _rule_show_popup(GtkWidget *widget, dt_lib_filtering_rule_t *rule, dt_lib_module_t *self)
{
  if(rule && rule->manual_widget_set) return TRUE;

  GCallback callback;
  if(rule)
    callback = G_CALLBACK(_event_rule_change_type);
  else
    callback = G_CALLBACK(_event_append_rule);

#define ADD_COLLECT_ENTRY(menu, value)                                                                            \
  _popup_add_item(menu, dt_collection_name(value), value, FALSE, callback, rule, self);

  // we show a popup with all the possible rules
  GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(spop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(spop), 200, -1);

  // the differents categories
  /*_popup_add_item(spop, _("files"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOLDERS);*/
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILENAME);

  _popup_add_item(spop, _("metadata"), 0, TRUE, NULL, NULL, self);
  /*ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
    {
      ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_METADATA + i);
    }
  }*/
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_RATING);
  /*ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GEOTAGGING);*/

  _popup_add_item(spop, _("times"), 0, TRUE, NULL, NULL, self);
  // ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TIME);
  /*ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_PRINT_TIMESTAMP);*/

  _popup_add_item(spop, _("capture details"), 0, TRUE, NULL, NULL, self);
  /*ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LENS);*/
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ISO);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ASPECT_RATIO);

  /*_popup_add_item(spop, _("darktable"), 0, TRUE, NULL, NULL, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ORDER);*/

  dt_gui_menu_popup(GTK_MENU(spop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

static gboolean _event_rule_append(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _rule_show_popup(widget, NULL, self);
  return TRUE;
}

static gboolean _event_rule_change_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  _rule_show_popup(rule->w_prop, rule, self);
  return TRUE;
}

// initialise or update a rule widget. Return if the a new widget has been created
static gboolean _widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                             const gchar *text, const dt_lib_collect_mode_t mode, gboolean off, const int pos,
                             dt_lib_module_t *self)
{
  rule->manual_widget_set++;

  const gboolean newmain = (rule->w_main == NULL);
  const gboolean newprop = (prop != rule->prop);
  GtkWidget *hbox = NULL;

  rule->prop = prop;

  if(newmain)
  {
    // the main box
    rule->w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(rule->w_main, "collect-rule-widget");

    // the first line
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), hbox, TRUE, TRUE, 0);
    gtk_widget_set_name(hbox, "collect-header-box");

    // operator type
    rule->w_operator = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("and"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("or"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("and not"));
    gtk_widget_set_name(rule->w_operator, "collect-operator");
    gtk_widget_set_tooltip_text(rule->w_operator, _("define how this rule should interact with the previous one"));
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_operator, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(rule->w_operator), "changed", G_CALLBACK(_event_rule_changed), rule);
  }

  gtk_combo_box_set_active(GTK_COMBO_BOX(rule->w_operator), mode);

  // property
  if(newmain)
  {
    rule->w_prop = gtk_button_new_with_label(dt_collection_name(prop));
    gtk_widget_set_name(rule->w_prop, "collect-property");
    gtk_widget_set_tooltip_text(rule->w_prop, _("rule property"));
    g_object_set_data(G_OBJECT(rule->w_prop), "rule", rule);
    GtkWidget *lb = gtk_bin_get_child(GTK_BIN(rule->w_prop));
    gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
    g_signal_connect(G_OBJECT(rule->w_prop), "button-press-event", G_CALLBACK(_event_rule_change_popup), self);
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_prop, TRUE, TRUE, 0);
  }
  else if(newprop)
  {
    gtk_button_set_label(GTK_BUTTON(rule->w_prop), dt_collection_name(prop));
  }

  if(newmain)
  {
    // in order to ensure the property is correctly centered, we add an invisible widget at the right
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *false_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("and"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("or"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("and not"));
    gtk_widget_set_sensitive(false_cb, FALSE);
    gtk_widget_set_name(false_cb, "collect-operator");
    gtk_container_add(GTK_CONTAINER(overlay), false_cb);
    gtk_box_pack_start(GTK_BOX(hbox), overlay, FALSE, FALSE, 0);


    GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(hbox2, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), hbox2);

    // on-off button
    rule->w_off = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
    gtk_widget_set_name(rule->w_off, "module-enable-button");
    gtk_widget_set_tooltip_text(rule->w_off, _("disable this collect rule"));
    g_signal_connect(G_OBJECT(rule->w_off), "toggled", G_CALLBACK(_event_rule_changed), rule);
    gtk_box_pack_end(GTK_BOX(hbox2), rule->w_off, FALSE, FALSE, 0);

    // remove button
    rule->w_close = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_name(GTK_WIDGET(rule->w_close), "basics-link");
    g_object_set_data(G_OBJECT(rule->w_close), "rule", rule);
    gtk_widget_set_tooltip_text(rule->w_close, _("remove this collect rule"));
    g_signal_connect(G_OBJECT(rule->w_close), "button-press-event", G_CALLBACK(_event_rule_close), self);
    gtk_box_pack_end(GTK_BOX(hbox2), rule->w_close, FALSE, FALSE, 0);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_off), !off);

  if(newmain)
  {
    // the second line
    rule->w_widget_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), rule->w_widget_box, TRUE, TRUE, 0);
    gtk_widget_set_name(rule->w_widget_box, "collect-module-hbox");
  }

  const gboolean newraw = g_strcmp0(text, rule->raw_text);

  _rule_set_raw_text(rule, text, FALSE);

  // initialize the specific entries if any
  if(newmain || newprop || newraw) _widget_init_special(rule, text, self);

  rule->manual_widget_set--;
  return newmain;
}

static void _filters_gui_update(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  ++darktable.gui->reset;
  d->nb_rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0, MAX_RULES);
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = 0; i < d->nb_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    const dt_collection_properties_t prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    const gchar *txt = dt_conf_get_string_const(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    const dt_lib_collect_mode_t rmode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    const int off = dt_conf_get_int(confname);
    if(_widget_init(&d->rule[i], prop, txt, rmode, off, i, self))
      gtk_box_pack_start(GTK_BOX(d->rules_box), d->rule[i].w_main, FALSE, TRUE, 0);
    gtk_widget_show_all(d->rule[i].w_main);
  }

  // remove all remaining rules
  for(int i = d->nb_rules; i < MAX_RULES; i++)
  {
    d->rule[i].prop = 0;
    if(d->rule[i].w_main)
    {
      gtk_widget_destroy(d->rule[i].w_main);
      d->rule[i].w_main = NULL;
      d->rule[i].w_special_box = NULL;
    }
  }

  --darktable.gui->reset;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/filtering/num_rules", 0);

  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

int position()
{
  return 380;
}

static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs, int next,
                                   gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;

  gchar *where_ext = dt_collection_get_extended_where(darktable.collection, 99999);
  if(g_strcmp0(where_ext, d->last_where_ext))
  {
    g_free(d->last_where_ext);
    d->last_where_ext = g_strdup(where_ext);
    for(int i = 0; i <= d->nb_rules; i++)
    {
      _widget_update(&d->rule[i]);
    }
  }
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog
      = gtk_dialog_new_with_buttons(_("collections settings"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                    _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  dt_prefs_init_dialog_collect(dialog);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void _history_pretty_print(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  char str[400] = { 0 };
  int mode, item, off;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    const int n = sscanf(buf, "%d:%d:%d:%399[^$]", &mode, &item, &off, str);

    if(n == 4)
    {
      if(k > 0)
      {
        switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = g_strlcpy(out, _(" and "), outsize);
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = g_strlcpy(out, _(" or "), outsize);
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = g_strlcpy(out, _(" but not "), outsize);
            out += c;
            outsize -= c;
            break;
        }
      }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      if(off)
      {
        c = snprintf(out, outsize, "%s%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                     _("(off)"), item == 0 ? dt_image_film_roll_name(str) : str);
      }
      else
      {
        c = snprintf(out, outsize, "%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                     item == 0 ? dt_image_film_roll_name(str) : str);
      }
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _event_history_apply(GtkWidget *widget, dt_lib_module_t *self)
{
  const int hid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history"));
  if(hid < 0 || hid >= dt_conf_get_int("plugins/lighttable/filtering/history_max")) return;

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", hid);
  const char *line = dt_conf_get_string_const(confname);
  if(line && line[0] != '\0')
  {
    dt_collection_deserialize(line, TRUE);
    _filters_gui_update(self);
  }
}

static gboolean _event_history_show(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  // we show a popup with all the history entries
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  const int maxitems = dt_conf_get_int("plugins/lighttable/filtering/history_max");

  for(int i = 0; i < maxitems; i++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    const char *line = dt_conf_get_string_const(confname);
    if(line && line[0] != '\0')
    {
      char str[2048] = { 0 };
      _history_pretty_print(line, str, sizeof(str));
      GtkWidget *smt = gtk_menu_item_new_with_label(str);
      gtk_widget_set_name(smt, "collect-popup-item");
      gtk_widget_set_tooltip_text(smt, str);
      // GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
      g_object_set_data(G_OBJECT(smt), "history", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_event_history_apply), self);
      gtk_menu_shell_append(pop, smt);
    }
    else
      break;
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

/* save the images order if the first collect filter is on tag*/
static void _sort_set_tag_order(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  if(darktable.collection->tagid)
  {
    const uint32_t sort = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(d->sort->sort));
    const gboolean descending = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->sort->direction));
    dt_tag_set_tag_order_by_id(darktable.collection->tagid, sort, descending);
  }
}

// set the sort order to the collection and update the query
static void _sort_update_query(dt_lib_module_t *self, gboolean update_filter)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const dt_collection_sort_t sort = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(d->sort->sort));
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->sort->direction));

  // if needed, we sync the filter bar
  if(update_filter) dt_view_filter_update_sort(darktable.view_manager, sort, reverse);

  // we update the collection
  dt_collection_set_sort(darktable.collection, sort, reverse);

  /* save the images order */
  _sort_set_tag_order(self);

  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);
}

// update the sort asc/desc arrow
static void _sort_update_arrow(GtkWidget *widget)
{
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(reverse)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN,
                                 NULL);
  else
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(widget);
}

static void _sort_reverse_changed(GtkDarktableToggleButton *widget, dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  if(d->manual_sort_set) return;

  _sort_update_arrow(GTK_WIDGET(widget));
  _sort_update_query(self, TRUE);
}

static void _sort_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  if(d->manual_sort_set) return;

  _sort_update_query(self, TRUE);
}

// this proxy function is primary called when the sort part of the filter bar is changed
static void _proxy_set_sort(dt_lib_module_t *self, dt_collection_sort_t sort, gboolean asc)
{
  // we update the widgets
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  d->manual_sort_set = TRUE;
  dt_bauhaus_combobox_set(d->sort->sort, sort);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->sort->direction), asc);
  _sort_update_arrow(d->sort->direction);
  d->manual_sort_set = FALSE;

  // we update the collection
  _sort_update_query(self, FALSE);
}

static _widgets_sort_t *_sort_get_widgets(dt_lib_module_t *self)
{
  _widgets_sort_t *wsort = (_widgets_sort_t *)g_malloc0(sizeof(_widgets_sort_t));
  wsort->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(wsort->box, "collect-sort-widget");
  const dt_collection_sort_t sort = dt_collection_get_sort_field(darktable.collection);
  wsort->sort = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("sort by"),
                                             _("determine the sort order of shown images"), sort,
                                             _sort_combobox_changed, self, NULL);

#define ADD_SORT_ENTRY(value)                                                                                     \
  dt_bauhaus_combobox_add_full(wsort->sort, dt_collection_sort_name(value), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,      \
                               GUINT_TO_POINTER(value), NULL, TRUE)

  ADD_SORT_ENTRY(DT_COLLECTION_SORT_FILENAME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DATETIME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CHANGE_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_EXPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PRINT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_RATING);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ID);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_COLOR);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_GROUP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PATH);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CUSTOM_ORDER);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_TITLE);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DESCRIPTION);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ASPECT_RATIO);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_SHUFFLE);

#undef ADD_SORT_ENTRY

  gtk_box_pack_start(GTK_BOX(wsort->box), wsort->sort, TRUE, TRUE, 0);

  /* reverse order checkbutton */
  wsort->direction = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_set_name(GTK_WIDGET(wsort->direction), "control-button");
  if(darktable.collection->params.descending)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(wsort->direction), dtgtk_cairo_paint_solid_arrow,
                                 CPF_DIRECTION_DOWN, NULL);
  gtk_widget_set_halign(wsort->direction, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(wsort->box), wsort->direction, FALSE, TRUE, 0);
  /* select the last value and connect callback */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wsort->direction),
                               dt_collection_get_sort_descending(darktable.collection));
  g_signal_connect(G_OBJECT(wsort->direction), "toggled", G_CALLBACK(_sort_reverse_changed), self);

  gtk_widget_show_all(wsort->box);

  return wsort;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)calloc(1, sizeof(dt_lib_filtering_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  d->nb_rules = 0;
  d->params = (dt_lib_filtering_params_t *)g_malloc0(sizeof(dt_lib_filtering_params_t));

  for(int i = 0; i < MAX_RULES; i++)
  {
    d->rule[i].num = i;
  }

  // the box to insert the collect rules
  d->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->rules_box, FALSE, TRUE, 0);

  // the botton buttons
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(spacer, "collect-spacer");
  gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);
  GtkWidget *bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(bhbox, "collect-actions-widget");
  gtk_box_set_homogeneous(GTK_BOX(bhbox), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), bhbox, TRUE, TRUE, 0);
  GtkWidget *btn = dt_ui_button_new(_("new rule"), _("append new rule to collect images"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_rule_append), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("history"), _("revert to a previous set of rules"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_history_show), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  gtk_widget_show_all(bhbox);

  // the sorting part
  spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(spacer, "collect-spacer2");
  bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);
  d->sort = _sort_get_widgets(self);
  gtk_box_pack_start(GTK_BOX(bhbox), d->sort->box, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("+"), _("add sort order"), NULL);
  // g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_history_show), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), bhbox, FALSE, TRUE, 0);

  /* setup proxy */
  darktable.view_manager->proxy.module_filtering.module = self;
  darktable.view_manager->proxy.module_filtering.update = _filters_gui_update;
  darktable.view_manager->proxy.module_filtering.set_sort = _proxy_set_sort;

  d->last_where_ext = dt_collection_get_extended_where(darktable.collection, 99999);
  _filters_gui_update(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_dt_collection_updated), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_updated), self);
  darktable.view_manager->proxy.module_filtering.module = NULL;
  free(d->params);

  /* TODO: Make sure we are cleaning up all allocations */

  free(self->data);
  self->data = NULL;
}

#undef MAX_RULES
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
