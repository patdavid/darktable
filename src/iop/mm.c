/*
 *  This file is part of darktable,
 *  copyright (c) 2016 Wolfgang Mader
 *  copyright (c) 2016 Maixmilian Trescher
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/locallaplacian.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_bw_params_t)

typedef enum _iop_operator_t
{
  OPERATOR_LIGHTNESS,
  OPERATOR_APPARENT_GRAYSCALE,
} _iop_operator_t;

typedef struct dt_iop_bw_params_t
{
  int32_t operator;
  float colorcontrast;
} dt_iop_bw_params_t;

typedef struct dt_iop_bw_gui_data_t
{
  GtkWidget *operator;
  GtkWidget *colorcontrast;
} dt_iop_bw_gui_data_t;

typedef struct dt_iop_bw_global_data_t
{
} dt_iop_bw_global_data_t;

const char *name()
{
  return _("modern monochrome");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

static inline void process_lightness(
    dt_dev_pixelpipe_iop_t *piece, const void *const ib, void *const ob,
    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;

  float *in;
  float *out;
  for(int j = 0; j < roi_out->height; ++j)
  {
    in  = ((float *)ib) + (size_t)ch * roi_in->width * j;
    out = ((float *)ob) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; ++i, in += ch, out += ch)
    {
      out[0] = in[0];
      out[1] = 0.0f;
      out[2] = 0.0f;
    }
  }
}

static inline void process_apparent_grayscale(
    dt_dev_pixelpipe_iop_t *piece, const void *const ib, void *const ob,
    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
#define PI atan2f(0, -1)

  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  const int ch = piece->colors;

  // u' and v' of the white point, Luv color space.
  const float uv_prime_c[2] = { (4.0f * d50[0]) / (d50[0] + 15.0f * d50[1] + 3.0f * d50[2]),
                                (9.0f * d50[1]) / (d50[0] + 15.0f * d50[1] + 3.0f * d50[2]) };
  //-> The denominator is the same for u and v. Is it faster to calculate once, store, and read, or
  //-> to calculate twice?

  const float adapting_luminance = 20.0f;
  // dependency of the adapting luminance onto the Helmholtz-Kohlrausch effect
  const float k_br = 0.2717f * (6.469f + 6.362f * powf(adapting_luminance, 0.4495f))
                     / (6.469f + powf(adapting_luminance, 0.4495f));

  float XYZ[3];
  float uv_prime[2]; // as u', v', but for the actual color of the pixel.

  float theta; // hue angle
  float s_uv;  // chromaticity
  float q;     // model of the Helmholtz-Kohlrausch effect

  float *in;
  float *out;
  for(int j = 0; j < roi_out->height; ++j)
  {
    in  = ((float *)ib) + (size_t)ch * roi_in->width * j;
    out = ((float *)ob) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; ++i, in += ch, out += ch)
    {
      // Lab -> XYZ
      dt_Lab_to_XYZ(in, XYZ);

      // XYZ -> Luv
      uv_prime[0] = (4.0f * XYZ[0]) / (XYZ[0] + 15.0f * XYZ[1] + 3.0f * XYZ[2] + 1e-5f);
      uv_prime[1] = (9.0f * XYZ[1]) / (XYZ[0] + 15.0f * XYZ[1] + 3.0f * XYZ[2] + 1e-5f);

      // Calculate monochrome value.
      s_uv = 13.0f * sqrtf(MAX(0.0f, powf(uv_prime[0] - uv_prime_c[0], 2) + powf(uv_prime[1] - uv_prime_c[1], 2)));
      theta = atan2f(uv_prime[1] - uv_prime_c[1],
                     uv_prime[0] - uv_prime_c[0]); // FIXME Check for domain error, atan2f(0, 0)
      if(!(theta == theta)) theta = 0.0f;

      q = -0.01585f - 0.03016f * cosf(theta) - 0.04556f * cosf(2.0f * theta) - 0.02667f * cosf(3.0f * theta)
          - 0.00295 * cosf(4.0f * theta) + 0.14592f * sinf(theta) + 0.05084f * sinf(2.0f * theta)
          - 0.019f * sinf(3.0f * theta) - 0.00764f * sinf(4.0f * theta);

      // L channel is the same in Luv and Lab. Thus, L as calculated in LUV can be used is Lab.
      out[0] = (1.0f + (0.0872f * k_br - 0.134f * q) * s_uv) * in[0];
      out[1] = 0;
      out[2] = 0;
    }
  }
}

static inline void process_local_laplacian(
    dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_bw_params_t *d = (dt_iop_bw_params_t *)piece->data;
  local_laplacian((const float *const)i, (float *const)o,
      roi_out->width, roi_out->height, 0.1, 1.0, 1.0, d->colorcontrast);
}

void process(
    struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
    const void *const i, void *const o,
    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_bw_params_t *d = (dt_iop_bw_params_t *)piece->data;

  switch(d->operator)
  {
    case OPERATOR_LIGHTNESS:
      process_lightness(piece, i, o, roi_in, roi_out);
      break;

    case OPERATOR_APPARENT_GRAYSCALE:
      process_apparent_grayscale(piece, i, o, roi_in, roi_out);
      process_local_laplacian(piece, o, o, roi_out, roi_out);
      break;
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  module->default_enabled = 0;

  dt_iop_bw_params_t tmp = (dt_iop_bw_params_t){ OPERATOR_APPARENT_GRAYSCALE, 0.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_bw_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bw_params_t));
}

void init(dt_iop_module_t *module)
{
  module->data = NULL; // malloc(sizeof(dt_iop_bw_global_data_t));
  module->params = calloc(1, sizeof(dt_iop_bw_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_bw_params_t));
  module->priority = 630; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bw_params_t);
  module->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_bw_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

/** gui callbacks */
static void operator_callback(GtkWidget *combobox, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_bw_gui_data_t *g = (dt_iop_bw_gui_data_t *)self->gui_data;

  dt_iop_bw_params_t *p = (dt_iop_bw_params_t *)self->params;
  p->operator= dt_bauhaus_combobox_get(combobox);

  // hide operator-specific widgets
  gtk_widget_set_visible(g->colorcontrast, FALSE);

  // enable widgets of the selected operator
  if(p->operator== OPERATOR_APPARENT_GRAYSCALE)
    gtk_widget_set_visible(g->colorcontrast, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void callback_colorcontrast(GtkWidget *contrast, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_bw_params_t *p = (dt_iop_bw_params_t *)self->params;
  p->colorcontrast = dt_bauhaus_slider_get(contrast);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_bw_gui_data_t *g = (dt_iop_bw_gui_data_t *)self->gui_data;
  dt_iop_bw_params_t *p = (dt_iop_bw_params_t *)self->params;

  dt_bauhaus_combobox_set(g->operator, p->operator);
  dt_bauhaus_slider_set(g->colorcontrast, p->colorcontrast);

  // hide operator-specific widgets
  gtk_widget_set_visible(g->colorcontrast, FALSE);

  // enable widgets of the selected operator
  if(p->operator== OPERATOR_APPARENT_GRAYSCALE)
    gtk_widget_set_visible(g->colorcontrast, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_bw_gui_data_t));
  dt_iop_bw_gui_data_t *g = (dt_iop_bw_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* operator combobox */
  g->operator= dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->operator, NULL, _("operator"));

  dt_bauhaus_combobox_add(g->operator, _("lightness"));
  dt_bauhaus_combobox_add(g->operator, _("apparent grayscale"));

  gtk_widget_set_tooltip_text(g->operator, _("the conversion operator"));
  g_signal_connect(G_OBJECT(g->operator), "value-changed", G_CALLBACK(operator_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->operator), TRUE, TRUE, 0);

  /* color contrast settings */
  g->colorcontrast = dt_bauhaus_slider_new_with_range(self, -1, 4, 0.1, 0, 1);
  dt_bauhaus_widget_set_label(g->colorcontrast, NULL, _("color contrast"));
  gtk_widget_set_tooltip_text(g->colorcontrast,
                              _("increase the contrast between hues that result in a similar lightness by local transfer of the original contrast (including color information) to the monochrome image."));
  g_signal_connect(G_OBJECT(g->colorcontrast), "value-changed",
                   G_CALLBACK(callback_colorcontrast), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->colorcontrast, TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
