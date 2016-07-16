/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_GUI_CURVE_INFL .3f
#define DT_IOP_TONECURVE_RES 256
#define MAXNODES 20


DT_MODULE_INTROSPECTION(3, dt_iop_basecurve_params_t)

typedef struct dt_iop_basecurve_node_t
{
  float x;
  float y;
} dt_iop_basecurve_node_t;

typedef struct dt_iop_basecurve_params_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
  int exposure_fusion;    // number of exposure fusion steps
  float exposure_stops;   // number of stops between fusion images
} dt_iop_basecurve_params_t;

typedef struct dt_iop_basecurve_params2_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
} dt_iop_basecurve_params2_t;

typedef struct dt_iop_basecurve_params1_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
} dt_iop_basecurve_params1_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    dt_iop_basecurve_params1_t *o = (dt_iop_basecurve_params1_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_basecurve_params_t){ {
                                        { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                      },
                                      { 2, 3, 3 },
                                      { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE } };
    for(int k = 0; k < 6; k++) n->basecurve[0][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->basecurve[0][k].y = o->tonecurve_y[k];
    n->basecurve_nodes[0] = 6;
    n->basecurve_type[0] = CUBIC_SPLINE;
    return 0;
  }
  if(old_version == 1 && new_version == 3)
  {
    dt_iop_basecurve_params1_t *o = (dt_iop_basecurve_params1_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_basecurve_params_t){ {
                                        { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                      },
                                      { 2, 3, 3 },
                                      { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE } , 0, 0};
    for(int k = 0; k < 6; k++) n->basecurve[0][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->basecurve[0][k].y = o->tonecurve_y[k];
    n->basecurve_nodes[0] = 6;
    n->basecurve_type[0] = CUBIC_SPLINE;
    return 0;
  }
  if(old_version == 2 && new_version == 3)
  {
    dt_iop_basecurve_params2_t *o = (dt_iop_basecurve_params2_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_basecurve_params2_t));
    n->exposure_fusion = 0;
    n->exposure_stops = 0;
    return 0;
  }
  return 1;
}

static const char neutral[] = N_("neutral");
static const char canon_eos[] = N_("canon eos like");
static const char canon_eos_alt[] = N_("canon eos like alternate");
static const char nikon[] = N_("nikon like");
static const char nikon_alt[] = N_("nikon like alternate");
static const char sony_alpha[] = N_("sony alpha like");
static const char pentax[] = N_("pentax like");
static const char ricoh[] = N_("ricoh like");
static const char olympus[] = N_("olympus like");
static const char olympus_alt[] = N_("olympus like alternate");
static const char panasonic[] = N_("panasonic like");
static const char leica[] = N_("leica like");
static const char kodak_easyshare[] = N_("kodak easyshare like");
static const char konica_minolta[] = N_("konica minolta like");
static const char samsung[] = N_("samsung like");
static const char fujifilm[] = N_("fujifilm like");
static const char nokia[] = N_("nokia like");

typedef struct basecurve_preset_t
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min, iso_max;
  dt_iop_basecurve_params_t params;
  int autoapply;
  int filter;
} basecurve_preset_t;

#define m MONOTONE_HERMITE

static const basecurve_preset_t basecurve_camera_presets[] = {
  // copy paste your measured basecurve line at the top here, like so (note the exif data and the last 1):

  // nikon d750 by Edouard Gomez
  {"Nikon D750", "NIKON CORPORATION", "NIKON D750", 0, 51200, {{{{0.000000, 0.000000}, {0.018124, 0.026126}, {0.143357, 0.370145}, {0.330116, 0.730507}, {0.457952, 0.853462}, {0.734950, 0.965061}, {0.904758, 0.985699}, {1.000000, 1.000000}}}, {8}, {m}}, 0, 1},
  // contributed by Stefan Kauerauf
  {"Nikon D5100", "NIKON CORPORATION", "NIKON D5100", 0, 51200, {{{{0.000000, 0.000000}, {0.001113, 0.000506}, {0.002842, 0.001338}, {0.005461, 0.002470}, {0.011381, 0.006099}, {0.013303, 0.007758}, {0.034638, 0.041119}, {0.044441, 0.063882}, {0.070338, 0.139639}, {0.096068, 0.210915}, {0.137693, 0.310295}, {0.206041, 0.432674}, {0.255508, 0.504447}, {0.302770, 0.569576}, {0.425625, 0.726755}, {0.554526, 0.839541}, {0.621216, 0.882839}, {0.702662, 0.927072}, {0.897426, 0.990984}, {1.000000, 1.000000}}}, {20}, {m}}, 0, 1},
  // nikon d7000 by Edouard Gomez
  {"Nikon D7000", "NIKON CORPORATION", "NIKON D7000", 0, 51200, {{{{0.000000, 0.000000}, {0.001943, 0.003040}, {0.019814, 0.028810}, {0.080784, 0.210476}, {0.145700, 0.383873}, {0.295961, 0.654041}, {0.651915, 0.952819}, {1.000000, 1.000000}}}, {8}, {m}}, 0, 1},
  // nikon d7200 standard by Ralf Brown (firmware 1.00)
  {"Nikon D7200", "NIKON CORPORATION", "NIKON D7200", 0, 51200, {{{{0.000000, 0.000000}, {0.001604, 0.001334}, {0.007401, 0.005237}, {0.009474, 0.006890}, {0.017348, 0.017176}, {0.032782, 0.044336}, {0.048033, 0.086548}, {0.075803, 0.168331}, {0.109539, 0.273539}, {0.137373, 0.364645}, {0.231651, 0.597511}, {0.323797, 0.736475}, {0.383796, 0.805797}, {0.462284, 0.872247}, {0.549844, 0.918328}, {0.678855, 0.962361}, {0.817445, 0.990406}, {1.000000, 1.000000}}}, {18}, {m}}, 1, 1},
  // sony rx100m2 by Günther R.
  { "Sony DSC-RX100M2", "SONY", "DSC-RX100M2", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.015106, 0.008116 }, { 0.070077, 0.093725 }, { 0.107484, 0.170723 }, { 0.191528, 0.341093 }, { 0.257996, 0.458453 }, { 0.305381, 0.537267 }, { 0.326367, 0.569257 }, { 0.448067, 0.723742 }, { 0.509627, 0.777966 }, { 0.676751, 0.898797 }, { 1.000000, 1.000000 } } }, { 12 }, { m } }, 0, 1 },
  // contributed by matthias bodenbinder
  { "Canon EOS 6D", "Canon", "Canon EOS 6D", 0, 51200, { { { { 0.000000, 0.002917 }, { 0.000751, 0.001716 }, { 0.006011, 0.004438 }, { 0.020286, 0.021725 }, { 0.048084, 0.085918 }, { 0.093914, 0.233804 }, { 0.162284, 0.431375 }, { 0.257701, 0.629218 }, { 0.384673, 0.800332 }, { 0.547709, 0.917761 }, { 0.751315, 0.988132 }, { 1.000000, 0.999943 } } }, { 12 }, { m } }, 0, 1 },
  // contributed by Dan Torop
  { "Fujifilm X100S", "Fujifilm", "X100S", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.009145, 0.007905 }, { 0.026570, 0.032201 }, { 0.131526, 0.289717 }, { 0.175858, 0.395263 }, { 0.350981, 0.696899 }, { 0.614997, 0.959451 }, { 1.000000, 1.000000 } } }, { 8 }, { m } }, 0, 1 },
  { "Fujifilm X100T", "Fujifilm", "X100T", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.009145, 0.007905 }, { 0.026570, 0.032201 }, { 0.131526, 0.289717 }, { 0.175858, 0.395263 }, { 0.350981, 0.696899 }, { 0.614997, 0.959451 }, { 1.000000, 1.000000 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by Johannes Hanika
  { "Canon EOS 5D Mark II", "Canon", "Canon EOS 5D Mark II", 0, 51200, { { { { 0.000000, 0.000366 }, { 0.006560, 0.003504 }, { 0.027310, 0.029834 }, { 0.045915, 0.070230 }, { 0.206554, 0.539895 }, { 0.442337, 0.872409 }, { 0.673263, 0.971703 }, { 1.000000, 0.999832 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by chrik5
  { "Pentax K-5", "Pentax", "Pentax K-5", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.004754, 0.002208 }, { 0.009529, 0.004214 }, { 0.023713, 0.013508 }, { 0.031866, 0.020352 }, { 0.046734, 0.034063 }, { 0.059989, 0.052413 }, { 0.088415, 0.096030 }, { 0.136610, 0.190629 }, { 0.174480, 0.256484 }, { 0.205192, 0.307430 }, { 0.228896, 0.348447 }, { 0.286411, 0.428680 }, { 0.355314, 0.513527 }, { 0.440014, 0.607651 }, { 0.567096, 0.732791 }, { 0.620597, 0.775968 }, { 0.760355, 0.881828 }, { 0.875139, 0.960682 }, { 1.000000, 1.000000 } } }, { 20 }, { m } }, 0, 1 },
  // contributed by Togan Muftuoglu - ed: slope is too aggressive on shadows
  //{ "Nikon D90", "NIKON", "D90", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.015520, 0.012248 }, { 0.097950, 0.251013 }, { 0.301515, 0.621951 }, { 0.415513, 0.771384 }, { 0.547326, 0.843079 }, { 0.819769, 0.956678 }, { 1.000000, 1.000000 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by Edouard Gomez
  {"Nikon D90", "NIKON CORPORATION", "NIKON D90", 0, 51200, {{{{0.000000, 0.000000}, {0.011702, 0.012659}, {0.122918, 0.289973}, {0.153642, 0.342731}, {0.246855, 0.510114}, {0.448958, 0.733820}, {0.666759, 0.894290}, {1.000000, 1.000000}}}, {8}, {m}}, 0, 1},
  // contributed by Pascal Obry
  { "Nikon D800", "NIKON", "D800", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.001773, 0.001936 }, { 0.009671, 0.009693 }, { 0.016754, 0.020617 }, { 0.024884, 0.037309 }, { 0.048174, 0.107768 }, { 0.056932, 0.139532 }, { 0.085504, 0.233303 }, { 0.130378, 0.349747 }, { 0.155476, 0.405445 }, { 0.175245, 0.445918 }, { 0.217657, 0.516873 }, { 0.308475, 0.668608 }, { 0.375381, 0.754058 }, { 0.459858, 0.839909 }, { 0.509567, 0.881543 }, { 0.654394, 0.960877 }, { 0.783380, 0.999161 }, { 0.859310, 1.000000 }, { 1.000000, 1.000000 } } }, { 20 }, { m } }, 0, 1 },
};
static const int basecurve_camera_presets_cnt = sizeof(basecurve_camera_presets) / sizeof(basecurve_preset_t);

static const basecurve_preset_t basecurve_presets[] = {
  // smoother cubic spline curve
  { N_("cubic spline"), "", "", 0, 51200, { { { { 0.0, 0.0}, { 1.0, 1.0 }, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.} } }, { 2 }, { CUBIC_SPLINE } }, 0, 0 },
  { neutral,         "", "",                      0, 51200, { { { { 0.000000, 0.000000 }, { 0.005000, 0.002500 }, { 0.150000, 0.300000 }, { 0.400000, 0.700000 }, { 0.750000, 0.950000 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 0, 1 },
  { canon_eos,       "Canon", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.028226, 0.029677 }, { 0.120968, 0.232258 }, { 0.459677, 0.747581 }, { 0.858871, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { canon_eos_alt,   "Canon", "EOS 5D Mark",      0, 51200, { { { { 0.000000, 0.000000 }, { 0.026210, 0.029677 }, { 0.108871, 0.232258 }, { 0.350806, 0.747581 }, { 0.669355, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { nikon,           "NIKON", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.036290, 0.036532 }, { 0.120968, 0.228226 }, { 0.459677, 0.759678 }, { 0.858871, 0.983468 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { nikon_alt,       "NIKON", "D____",            0, 51200, { { { { 0.000000, 0.000000 }, { 0.012097, 0.007322 }, { 0.072581, 0.130742 }, { 0.310484, 0.729291 }, { 0.611321, 0.951613 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { sony_alpha,      "SONY", "",                  0, 51200, { { { { 0.000000, 0.000000 }, { 0.031949, 0.036532 }, { 0.105431, 0.228226 }, { 0.434505, 0.759678 }, { 0.855738, 0.983468 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { pentax,          "PENTAX", "",                0, 51200, { { { { 0.000000, 0.000000 }, { 0.032258, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { ricoh,           "RICOH", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.032259, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { olympus,         "OLYMPUS", "",               0, 51200, { { { { 0.000000, 0.000000 }, { 0.033962, 0.028226 }, { 0.249057, 0.439516 }, { 0.501887, 0.798387 }, { 0.750943, 0.955645 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { olympus_alt,     "OLYMPUS", "E-M",            0, 51200, { { { { 0.000000, 0.000000 }, { 0.012097, 0.010322 }, { 0.072581, 0.167742 }, { 0.310484, 0.711291 }, { 0.645161, 0.956855 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { panasonic,       "Panasonic", "",             0, 51200, { { { { 0.000000, 0.000000 }, { 0.036290, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { leica,           "Leica", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.036291, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { kodak_easyshare, "EASTMAN KODAK COMPANY", "", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.044355, 0.020967 }, { 0.133065, 0.154322 }, { 0.209677, 0.300301 }, { 0.572581, 0.753477 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { konica_minolta,  "MINOLTA", "",               0, 51200, { { { { 0.000000, 0.000000 }, { 0.020161, 0.010322 }, { 0.112903, 0.167742 }, { 0.500000, 0.711291 }, { 0.899194, 0.956855 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { samsung,         "SAMSUNG", "",               0, 51200, { { { { 0.000000, 0.000000 }, { 0.040323, 0.029677 }, { 0.133065, 0.232258 }, { 0.447581, 0.747581 }, { 0.842742, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { fujifilm,        "FUJIFILM", "",              0, 51200, { { { { 0.000000, 0.000000 }, { 0.028226, 0.029677 }, { 0.104839, 0.232258 }, { 0.387097, 0.747581 }, { 0.754032, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { nokia,           "Nokia", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.041825, 0.020161 }, { 0.117871, 0.153226 }, { 0.319392, 0.500000 }, { 0.638783, 0.842742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
};
#undef m
static const int basecurve_presets_cnt = sizeof(basecurve_presets) / sizeof(basecurve_preset_t);

typedef struct dt_iop_basecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve; // curve for gui to draw
  int minmax_curve_type, minmax_curve_nodes;
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkWidget *scale, *fusion;
  double mouse_x, mouse_y;
  int selected;
  double selected_offset, selected_y, selected_min, selected_max;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
  int loglogscale;
} dt_iop_basecurve_gui_data_t;

typedef struct dt_iop_basecurve_data_t
{
  dt_draw_curve_t *curve; // curve for pixelpipe piece and pixel processing
  int basecurve_type;
  int basecurve_nodes;
  float table[0x10000];      // precomputed look-up table for tone curve
  float unbounded_coeffs[3]; // approximation for extrapolation
  int exposure_fusion;
  float exposure_stops;
} dt_iop_basecurve_data_t;

typedef struct dt_iop_basecurve_global_data_t
{
  int kernel_basecurve;
} dt_iop_basecurve_global_data_t;


const char *name()
{
  return _("base curve");
}

int groups()
{
  return IOP_GROUP_BASIC;
}


int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

static void set_presets(dt_iop_module_so_t *self, const basecurve_preset_t *presets, int count, int *force_autoapply)
{
  // transform presets above to db entries.
  for(int k = 0; k < count; k++)
  {
    // add the preset.
    dt_gui_presets_add_generic(_(presets[k].name), self->op, self->version(),
                               &presets[k].params, sizeof(dt_iop_basecurve_params_t), 1);
    // and restrict it to model, maker, iso, and raw images
    dt_gui_presets_update_mml(_(presets[k].name), self->op, self->version(),
                              presets[k].maker, presets[k].model, "");
    dt_gui_presets_update_iso(_(presets[k].name), self->op, self->version(),
                              presets[k].iso_min, presets[k].iso_max);
    dt_gui_presets_update_ldr(_(presets[k].name), self->op, self->version(), FOR_RAW);
    // make it auto-apply for matching images:
    dt_gui_presets_update_autoapply(_(presets[k].name), self->op, self->version(),
                                    force_autoapply ? *force_autoapply : presets[k].autoapply);
    // hide all non-matching presets in case the model string is set.
    // When force_autoapply was given always filter (as these are per-camera presets)
    dt_gui_presets_update_filter(_(presets[k].name), self->op, self->version(),
                                 force_autoapply || presets[k].filter);
  }
}

void init_presets(dt_iop_module_so_t *self)
{
  // sql begin
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  set_presets(self, basecurve_presets, basecurve_presets_cnt, NULL);
  int force_autoapply = dt_conf_get_bool("plugins/darkroom/basecurve/auto_apply_percamera_presets");
  set_presets(self, basecurve_camera_presets, basecurve_camera_presets_cnt, &force_autoapply);

  // sql commit
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)piece->data;
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->data;

  cl_mem dev_m = NULL;
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dev_m = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_m == NULL) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 5, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_basecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static inline void apply_ev_and_curve(
    const float *const in,
    float *const out,
    const int width,
    const int height,
    const float mul,
    const float *const table,
    const float *const unbounded_coeffs)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    const float *inp = in + 4 * k;
    float *outp = out + 4 * k;
    for(int i = 0; i < 3; i++)
    {
      const float f = inp[i] * mul;
      // use base curve for values < 1, else use extrapolation.
      if(f < 1.0f)
        outp[i] = table[CLAMP((int)(f * 0x10000ul), 0, 0xffff)];
      else if(unbounded_coeffs)
        outp[i] = dt_iop_eval_exp(unbounded_coeffs, f);
      else outp[i] = 1.0f;
    }
  }
}

static inline void compute_features(
    float *const col,
    const int wd,
    const int ht)
{
  // features are product of
  // 1) well exposedness
  // 2) saturation
  // 3) local contrast (handled in laplacian form later)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j=0;j<ht;j++) for(int i=0;i<wd;i++)
  {
    const size_t x = 4*(wd*j+i);
    const float max = MAX(col[x], MAX(col[x+1], col[x+2]));
    const float min = MIN(col[x], MIN(col[x+1], col[x+2]));
    const float sat = .1f + 5.0f*(max-min)/MAX(1e-4, max);
    col[x+3] = sat;

    float v = fabsf(col[x]-0.5f);
    v = MAX(fabsf(col[x+1]-0.5f), v);
    v = MAX(fabsf(col[x+2]-0.5f), v);
    const float var = 0.4;
    const float exp = .1f + expf(-v*v/(var*var));
    col[x+3] *= exp;
  }
}

static inline void gauss_blur(
    const float *const input,
    float *const output,
    const int wd,
    const int ht)
{
  const float w[5] = {1./16., 4./16., 6./16., 4./16., 1./16.};
  float *tmp = dt_alloc_align(64, wd*ht*4*sizeof(float));
  memset(tmp, 0, 4*wd*ht*sizeof(float));
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(tmp)
#endif
  for(int j=0;j<ht;j++)
  { // horizontal pass
    // left borders
    for(int i=0;i<2;i++) for(int c=0;c<4;c++)
      for(int ii=-2;ii<=2;ii++)
        tmp[4*(j*wd+i)+c] += input[4*(j*wd+MAX(-i-ii,i+ii))+c] * w[ii+2];
    // most pixels
    for(int i=2;i<wd-2;i++) for(int c=0;c<4;c++)
      for(int ii=-2;ii<=2;ii++)
        tmp[4*(j*wd+i)+c] += input[4*(j*wd+i+ii)+c] * w[ii+2];
    // right borders
    for(int i=wd-2;i<wd;i++) for(int c=0;c<4;c++)
      for(int ii=-2;ii<=2;ii++)
        tmp[4*(j*wd+i)+c] += input[4*(j*wd+MIN(i+ii, wd-(i+ii-wd+1) ))+c] * w[ii+2];
  }
  memset(output, 0, 4*wd*ht*sizeof(float));
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(tmp)
#endif
  for(int i=0;i<wd;i++)
  { // vertical pass
    for(int j=0;j<2;j++) for(int c=0;c<4;c++)
      for(int jj=-2;jj<=2;jj++)
        output[4*(j*wd+i)+c] += tmp[4*(MAX(-j-jj,j+jj)*wd+i)+c] * w[jj+2];
    for(int j=2;j<ht-2;j++) for(int c=0;c<4;c++)
      for(int jj=-2;jj<=2;jj++)
        output[4*(j*wd+i)+c] += tmp[4*((j+jj)*wd+i)+c] * w[jj+2];
    for(int j=ht-2;j<ht;j++) for(int c=0;c<4;c++)
      for(int jj=-2;jj<=2;jj++)
        output[4*(j*wd+i)+c] += tmp[4*(MIN(j+jj, ht-(j+jj-ht+1))*wd+i)+c] * w[jj+2];
  }
  free(tmp);
}

static inline void gauss_expand(
    const float *const input, // coarse input
    float *const fine,        // upsampled, blurry output
    const int wd,             // fine res
    const int ht)
{
  const int cw = (wd-1)/2+1;
  // fill numbers in even pixels, zero odd ones
  memset(fine, 0, 4*wd*ht*sizeof(float));
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j=0;j<ht;j+=2)
    for(int i=0;i<wd;i+=2)
      for(int c=0;c<4;c++)
        fine[4*(j*wd+i)+c] = 4.0f * input[4*(j/2*cw + i/2)+c];

  // convolve with same kernel weights mul by 4:
  gauss_blur(fine, fine, wd, ht);
}

static inline void gauss_reduce(
    const float *const input, // fine input buffer
    float *const coarse,      // coarse scale, blurred input buf
    float *const detail,      // detail/laplacian, fine scale, or 0
    const int wd,
    const int ht)
{
  // blur, store only coarse res
  const int cw = (wd-1)/2+1;

  // TODO: pass in output buffer as tmp?
  float *blurred = dt_alloc_align(64, wd*ht*4*sizeof(float));
  gauss_blur(input, blurred, wd, ht);
  for(int j=0;2*j<ht;j++) for(int i=0;2*i<wd;i++)
    for(int c=0;c<4;c++) coarse[4*(j*cw+i)+c] = blurred[4*(2*j*wd+2*i)+c];
  free(blurred);

#if 0
  const int cw = (wd-1)/2+1, ch = (ht-1)/2+1;
  const float w[5] = {1./16., 4./16., 6./16., 4./16., 1./16.};
  // TODO: border handling!
  for(int j=1;2*j+2<ht;j++) for(int i=1;2*i+2<wd;i++)
  { // keep even pixels only
    const int ind = 4*(j*cw+i), indi = 4*(2*j*wd+2*i);
    for(int c=0;c<4;c++) coarse[ind + c] = 0.0f;
    for(int ii=-2;ii<=2;ii++)
      for(int jj=-2;jj<=2;jj++)
        for(int c=0;c<4;c++)
          coarse[ind + c] +=
            input[indi+4*(wd*jj+ii)+c]*w[jj+2]*w[ii+2];
  }
#endif
  if(detail)
  {
    // compute laplacian/details: expand coarse buffer into detail
    // buffer subtract expanded buffer from input in place
    gauss_expand(coarse, detail, wd, ht);
    for(int64_t k=0;k<wd*ht*4;k++)
      detail[k] = input[k] - detail[k];
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const float *const in  = (float *)i;
  float *const out = (float *)o;
  const int ch = piece->colors;
  dt_iop_basecurve_data_t *const d = (dt_iop_basecurve_data_t *)(piece->data);

  // are we doing exposure fusion?
  if(d->exposure_fusion)
  {
    // allocate temporary buffer for wavelet transform + blending
    const int wd = roi_in->width, ht = roi_in->height;
    int num_levels = 8;
    float **col  = malloc(num_levels * sizeof(float*));
    float **comb = malloc(num_levels * sizeof(float*));
    int w = wd, h = ht;
    const int rad = MIN(wd, ceilf(256 * roi_in->scale / piece->iscale));
    int step = 1;
    for(int k=0;k<num_levels;k++)
    {
      // coarsest step is some % of image width.
      col[k]  = dt_alloc_align(64, sizeof(float)*4*w*h);
      comb[k] = dt_alloc_align(64, sizeof(float)*4*w*h);
      memset(comb[k], 0, sizeof(float)*4*w*h);
      w = (w-1)/2+1; h = (h-1)/2+1;
      step *= 2;
      if(step > rad || w < 4 || h < 4)
      {
        num_levels = k+1;
        break;
      }
    }

    for(int e=0;e<d->exposure_fusion+1;e++)
    { // for every exposure fusion image:
      // push by some ev, apply base curve:
      apply_ev_and_curve(
          in, col[0], wd, ht,
          powf(2.0f, d->exposure_stops * e),
          d->table, d->unbounded_coeffs);

      // compute features
      compute_features(col[0], wd, ht);

      // create gaussian pyramid of colour buffer
      int w = wd; h = ht;
      gauss_reduce(col[0], col[1], out, w, h);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(col) schedule(static)
#endif
      for(size_t k=0;k<4*wd*ht;k+=4)
        col[0][k+3] *= .8f + out[k]*out[k] + out[k+1]*out[k+1] + out[k+2]*out[k+2];

// #define DEBUG_VIS
#ifdef DEBUG_VIS // DEBUG visualise weight buffer
      for(size_t k=0;k<4*w*h;k+=4)
        comb[0][k+e] = col[0][k+3];
      continue;
#endif

      for(int k=1;k<num_levels;k++)
      {
        gauss_reduce(col[k-1], col[k], 0, w, h);
        w = (w-1)/2+1; h = (h-1)/2+1;
      }

      // update pyramid coarse to fine
      for(int k=num_levels-1;k>=0;k--)
      {
        w = wd; h = ht;
        for(int i=0;i<k;i++) { w = (w-1)/2+1; h = (h-1)/2+1; }
        // abuse output buffer as temporary memory:
        if(k!=num_levels-1)
          gauss_expand(col[k+1], out, w, h);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(col,comb,w,h,num_levels,k) schedule(static)
#endif
        for(int j=0;j<h;j++) for(int i=0;i<w;i++)
        {
          const size_t x = 4*(w*j+i);
          // blend images into output pyramid
          if(k == num_levels-1) // blend gaussian base
            for(int c=0;c<3;c++)
              comb[k][x+c] += col[k][x+3] * col[k][x+c];
          else // laplacian
            for(int c=0;c<3;c++) comb[k][x+c] +=
              col[k][x+3] * (col[k][x+c] - out[x+c]);
          comb[k][x+3] += col[k][x+3];
        }
      }
    }

#ifndef DEBUG_VIS // DEBUG: switch off when visualising weight buf
    // normalise and reconstruct output pyramid buffer coarse to fine
    for(int k=num_levels-1;k>=0;k--)
    {
      int w = wd, h = ht;
      for(int i=0;i<k;i++) { w = (w-1)/2+1; h = (h-1)/2+1;}

      // normalise both gaussian base and laplacians:
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(comb,w,h,k) schedule(static)
#endif
      for(size_t i=0;i<(size_t)4*w*h;i+=4)
        if(comb[k][i+3] > 1e-8f)
          for(int c=0;c<3;c++) comb[k][i+c] /= comb[k][i+3];

      if(k < num_levels-1)
      { // reconstruct output image
        gauss_expand(comb[k+1], out, w, h);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(comb,w,h,k) schedule(static)
#endif
        for(int j=0;j<h;j++) for(int i=0;i<w;i++)
        {
          const size_t x = 4*(w*j+i);
          for(int c=0;c<3;c++) comb[k][x+c] += out[x+c];
        }
      }
    }
#endif

    // copy output buffer
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(comb) schedule(static)
#endif
    for(int k=0;k<4*wd*ht;k+=4)
    {
      out[k+0] = comb[0][k+0];
      out[k+1] = comb[0][k+1];
      out[k+2] = comb[0][k+2];
      out[k+3] = in[k+3]; // pass on 4th channel
    }

    // free temp buffers
    for(int k=0;k<num_levels;k++)
    {
      free(col[k]);
      free(comb[k]);
    }
    free(col);
    free(comb);
    return;
  }

  // fast path for non-fusion
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    const float *inp = in + ch * k;
    float *outp = out + ch * k;
    for(int i = 0; i < 3; i++)
    {
      // use base curve for values < 1, else use extrapolation.
      if(inp[i] < 1.0f)
        outp[i] = d->table[CLAMP((int)(inp[i] * 0x10000ul), 0, 0xffff)];
      else
        outp[i] = dt_iop_eval_exp(d->unbounded_coeffs, inp[i]);
    }

    outp[3] = inp[3];
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)p1;

  // TODO: implement opencl version:
  if(p->exposure_fusion) piece->process_cl_ready = 0;
  d->exposure_fusion = p->exposure_fusion;
  d->exposure_stops = p->exposure_stops;

  const int ch = 0;
  // take care of possible change of curve type or number of nodes (not yet implemented in UI)
  if(d->basecurve_type != p->basecurve_type[ch] || d->basecurve_nodes != p->basecurve_nodes[ch])
  {
    if(d->curve) // catch initial init_pipe case
      dt_draw_curve_destroy(d->curve);
    d->curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[ch]);
    d->basecurve_nodes = p->basecurve_nodes[ch];
    d->basecurve_type = p->basecurve_type[ch];
    for(int k = 0; k < p->basecurve_nodes[ch]; k++)
    {
      // printf("p->basecurve[%i][%i].x = %f;\n", ch, k, p->basecurve[ch][k].x);
      // printf("p->basecurve[%i][%i].y = %f;\n", ch, k, p->basecurve[ch][k].y);
      (void)dt_draw_curve_add_point(d->curve, p->basecurve[ch][k].x, p->basecurve[ch][k].y);
    }
  }
  else
  {
    for(int k = 0; k < p->basecurve_nodes[ch]; k++)
      dt_draw_curve_set_point(d->curve, k, p->basecurve[ch][k].x, p->basecurve[ch][k].y);
  }
  dt_draw_curve_calc_values(d->curve, 0.0f, 1.0f, 0x10000, NULL, d->table);

  // now the extrapolation stuff:
  const float xm = p->basecurve[0][p->basecurve_nodes[0] - 1].x;
  const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
  const float y[4] = { d->table[CLAMP((int)(x[0] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[1] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[2] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  piece->data = calloc(1, sizeof(dt_iop_basecurve_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->fusion, p->exposure_fusion);
  // gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_basecurve_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_basecurve_params_t));
  module->default_enabled = 0;
  module->priority = 307; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_basecurve_params_t);
  module->gui_data = NULL;
  dt_iop_basecurve_params_t tmp = (dt_iop_basecurve_params_t){
    {
      { // three curves (L, a, b) with a number of nodes
        { 0.0, 0.0 },
        { 1.0, 1.0 }
      },
    },
    { 2, 0, 0 }, // number of nodes per curve
    { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_basecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_basecurve_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_basecurve_global_data_t *gd
      = (dt_iop_basecurve_global_data_t *)malloc(sizeof(dt_iop_basecurve_global_data_t));
  module->data = gd;
  gd->kernel_basecurve = dt_opencl_create_kernel(program, "basecurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_basecurve);
  free(module->data);
  module->data = NULL;
}

static gboolean dt_iop_basecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  c->mouse_x = fabs(c->mouse_x);
  c->mouse_y = fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  // sign swapping for fluxbox
  c->mouse_x = -fabs(c->mouse_x);
  c->mouse_y = -fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static float to_log(const float x, float base)
{
  if(base)
    return logf(x * (base - 1.0f) + 1.0f) / logf(base);
  else
    return x;
}
static float to_lin(const float x, float base)
{
  if(base)
    return (powf(base, x) - 1.0f) / (base - 1.0f);
  else
    return x;
}

static gboolean dt_iop_basecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  int nodes = p->basecurve_nodes[0];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[0];
  if(c->minmax_curve_type != p->basecurve_type[0] || c->minmax_curve_nodes != p->basecurve_nodes[0])
  {
    dt_draw_curve_destroy(c->minmax_curve);
    c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
    c->minmax_curve_nodes = p->basecurve_nodes[0];
    c->minmax_curve_type = p->basecurve_type[0];
    for(int k = 0; k < p->basecurve_nodes[0]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  }
  else
  {
    for(int k = 0; k < p->basecurve_nodes[0]; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p->basecurve[0][k].x, p->basecurve[0][k].y);
  }
  dt_draw_curve_t *minmax_curve = c->minmax_curve;
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  const float xm = basecurve[nodes - 1].x;
  const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
  const float y[4] = { c->draw_ys[CLAMP((int)(x[0] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                       c->draw_ys[CLAMP((int)(x[1] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                       c->draw_ys[CLAMP((int)(x[2] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                       c->draw_ys[CLAMP((int)(x[3] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)] };
  float unbounded_coeffs[3];
  dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0; k<inset; k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.6f;
    cairo_fill(cr);
  }
#else
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
#endif

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1.0f, -1.0f);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  if(c->loglogscale)
    dt_draw_loglog_grid(cr, 4, 0, 0, width, height, c->loglogscale);
  else
    dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  for(int k = 0; k < nodes; k++)
  {
    const float x = to_log(basecurve[k].x, c->loglogscale), y = to_log(basecurve[k].y, c->loglogscale);
    cairo_arc(cr, x * width, y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  if(c->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = to_log(basecurve[c->selected].x, c->loglogscale),
                y = to_log(basecurve[c->selected].y, c->loglogscale);
    cairo_arc(cr, x * width, y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, height * to_log(c->draw_ys[0], c->loglogscale));
  for(int k = 1; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0);
    if(xx > xm)
    {
      const float yy = dt_iop_eval_exp(unbounded_coeffs, xx);
      const float x = to_log(xx, c->loglogscale), y = to_log(yy, c->loglogscale);
      cairo_line_to(cr, x * width, height * y);
    }
    else
    {
      const float yy = c->draw_ys[k];
      const float x = to_log(xx, c->loglogscale), y = to_log(yy, c->loglogscale);
      cairo_line_to(cr, x * width, height * y);
    }
  }
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static inline int _add_node(dt_iop_basecurve_node_t *basecurve, int *nodes, float x, float y)
{
  int selected = -1;
  if(basecurve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(basecurve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    basecurve[i].x = basecurve[i - 1].x;
    basecurve[i].y = basecurve[i - 1].y;
  }
  // found a new point
  basecurve[selected].x = x;
  basecurve[selected].y = y;
  (*nodes)++;
  return selected;
}

static gboolean dt_iop_basecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  const float mx = c->mouse_x / (float)width;
  const float my = 1.0f - c->mouse_y / (float)height;
  const float linx = to_lin(mx, c->loglogscale), liny = to_lin(my, c->loglogscale);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      basecurve[c->selected].x = linx;
      basecurve[c->selected].y = liny;

      // delete vertex if order has changed:
      if(nodes > 2)
        if((c->selected > 0 && basecurve[c->selected - 1].x >= linx)
           || (c->selected < nodes - 1 && basecurve[c->selected + 1].x <= linx))
        {
          for(int k = c->selected; k < nodes - 1; k++)
          {
            basecurve[k].x = basecurve[k + 1].x;
            basecurve[k].y = basecurve[k + 1].y;
          }
          c->selected = -2; // avoid re-insertion of that point immediately after this
          p->basecurve_nodes[ch]--;
        }
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    else if(nodes < MAXNODES && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      c->selected = _add_node(basecurve, &p->basecurve_nodes[ch], linx, liny);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = .04f;
    min *= min; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      float dist
          = (my - to_log(basecurve[k].y, c->loglogscale)) * (my - to_log(basecurve[k].y, c->loglogscale))
            + (mx - to_log(basecurve[k].x, c->loglogscale)) * (mx - to_log(basecurve[k].x, c->loglogscale));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_params_t *d = (dt_iop_basecurve_params_t *)self->default_params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK
      && nodes < MAXNODES && c->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
      c->mouse_x = CLAMP(event->x - inset, 0, width);
      c->mouse_y = CLAMP(event->y - inset, 0, height);

      const float mx = c->mouse_x / (float)width;
      const float linx = to_lin(mx, c->loglogscale);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(basecurve[0].x > linx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(basecurve[k].x > linx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;
      // > 0 -> check distance to left neighbour
      // < nodes -> check distance to right neighbour
      if(!((selected > 0 && linx - basecurve[selected - 1].x <= 0.025) ||
           (selected < nodes && basecurve[selected].x - linx <= 0.025)))
      {
        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(c->minmax_curve, linx);

        if(y >= 0.0 && y <= 1.0) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          int selected = _add_node(basecurve, &p->basecurve_nodes[ch], linx, y);

          // maybe set the new one as being selected
          float min = .04f;
          min *= min; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            float other_y = to_log(basecurve[k].y, c->loglogscale);
            float dist = (y - other_y) * (y - other_y);
            if(dist < min) c->selected = selected;
          }

          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset current curve
      p->basecurve_nodes[ch] = d->basecurve_nodes[ch];
      p->basecurve_type[ch] = d->basecurve_type[ch];
      for(int k = 0; k < d->basecurve_nodes[ch]; k++)
      {
        p->basecurve[ch][k].x = d->basecurve[ch][k].x;
        p->basecurve[ch][k].y = d->basecurve[ch][k].y;
      }
      c->selected = -2; // avoid motion notify re-inserting immediately.
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean area_resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkRequisition r;
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  int ch = 0;
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(c->selected >= 0)
  {
    if(event->direction == GDK_SCROLL_UP)
      basecurve[c->selected].y = MAX(0.0f, basecurve[c->selected].y + 0.001f);
    if(event->direction == GDK_SCROLL_DOWN)
      basecurve[c->selected].y = MIN(1.0f, basecurve[c->selected].y - 0.001f);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}

static void fusion_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  int fuse = dt_bauhaus_combobox_get(widget);
  p->exposure_fusion = fuse;
  p->exposure_stops = 3;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void scale_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  if(dt_bauhaus_combobox_get(widget))
    g->loglogscale = 64;
  else
    g->loglogscale = 0;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_basecurve_gui_data_t));
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
  c->minmax_curve_type = p->basecurve_type[0];
  c->minmax_curve_nodes = p->basecurve_nodes[0];
  for(int k = 0; k < p->basecurve_nodes[0]; k++)
    (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->loglogscale = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_widget_set_tooltip_text(GTK_WIDGET(c->area), _("abscissa: input, ordinate: output. works on RGB channels"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  c->scale = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->scale, NULL, _("scale"));
  dt_bauhaus_combobox_add(c->scale, _("linear"));
  dt_bauhaus_combobox_add(c->scale, _("logarithmic"));
  gtk_widget_set_tooltip_text(c->scale, _("scale to use in the graph. use logarithmic scale for "
                                          "more precise control near the blacks"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->scale, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->scale), "value-changed", G_CALLBACK(scale_callback), self);

  c->fusion = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->fusion, NULL, _("fusion"));
  dt_bauhaus_combobox_add(c->fusion, _("none"));
  dt_bauhaus_combobox_add(c->fusion, _("0ev, +3ev"));
  dt_bauhaus_combobox_add(c->fusion, _("0ev, +3ev, +6ev"));
  gtk_widget_set_tooltip_text(c->fusion, _("fuse this image stopped up a couple of times with itself, to compress high dynamic range. expose for the highlights before use."));
  gtk_box_pack_start(GTK_BOX(self->widget), c->fusion, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->fusion), "value-changed", G_CALLBACK(fusion_callback), self);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_basecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_basecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_basecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_basecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_basecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
