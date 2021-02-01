/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include <stdint.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"

#define HISTOGRAM_BINS 256


DT_MODULE(1)

typedef enum dt_lib_histogram_highlight_t
{
  DT_LIB_HISTOGRAM_HIGHLIGHT_NONE = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE
} dt_lib_histogram_highlight_t;

typedef enum dt_lib_histogram_scope_type_t
{
  DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM = 0,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM,
  DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE,
  DT_LIB_HISTOGRAM_SCOPE_N // needs to be the last one
} dt_lib_histogram_scope_type_t;

typedef enum dt_lib_histogram_scale_t
{
  DT_LIB_HISTOGRAM_LOGARITHMIC = 0,
  DT_LIB_HISTOGRAM_LINEAR,
  DT_LIB_HISTOGRAM_N // needs to be the last one
} dt_lib_histogram_scale_t;

typedef enum dt_lib_histogram_waveform_type_t
{
  DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID = 0,
  DT_LIB_HISTOGRAM_WAVEFORM_PARADE,
  DT_LIB_HISTOGRAM_WAVEFORM_N // needs to be the last one
} dt_lib_histogram_waveform_type_t;

typedef enum dt_lib_histogram_vectorscope_type_t
{
  DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV = 0,   // CIE 1976 u*v*
  DT_LIB_HISTOGRAM_VECTORSCOPE_JZAZBZ,
  DT_LIB_HISTOGRAM_VECTORSCOPE_N // needs to be the last one
} dt_lib_histogram_vectorscope_type_t;

// FIXME: are these lists available from the enum/options in darktableconfig.xml?
const gchar *dt_lib_histogram_scope_type_names[DT_LIB_HISTOGRAM_SCOPE_N] = { "histogram", "waveform", "vectorscope" };
const gchar *dt_lib_histogram_histogram_scale_names[DT_LIB_HISTOGRAM_N] = { "logarithmic", "linear" };
const gchar *dt_lib_histogram_waveform_type_names[DT_LIB_HISTOGRAM_WAVEFORM_N] = { "overlaid", "parade" };
const gchar *dt_lib_histogram_vectorscope_type_names[DT_LIB_HISTOGRAM_VECTORSCOPE_N] = { "u*v*", "AzBz" };

typedef struct dt_lib_histogram_t
{
  // histogram for display
  uint32_t *histogram;
  uint32_t histogram_max;
  // waveform histogram buffer and dimensions
  float *waveform_linear, *waveform_display;
  uint8_t *waveform_8bit;
  int waveform_width, waveform_height, waveform_max_width;
  uint8_t *vectorscope_alpha;
  int vectorscope_diameter, vectorscope_alpha_stride;
  float vectorscope_graticule[6][2] DT_ALIGNED_PIXEL;
  dt_pthread_mutex_t lock;
  GtkWidget *scope_draw;               // GtkDrawingArea -- scope, scale, and draggable overlays
  GtkWidget *button_box;               // GtkButtonBox -- contains scope control buttons
  GtkWidget *scope_type_button;        // GtkButton -- histogram/waveform/vectorscope control
  GtkWidget *scope_view_button;        // GtkButton -- how to render the current scope
  GtkWidget *red_channel_button;       // GtkToggleButton -- enable/disable processing R channel
  GtkWidget *green_channel_button;     // GtkToggleButton -- enable/disable processing G channel
  GtkWidget *blue_channel_button;      // GtkToggleButton -- enable/disable processing B channel
  // drag to change parameters
  gboolean dragging;
  int32_t button_down_x, button_down_y;
  float button_down_value;
  // depends on mouse position
  dt_lib_histogram_highlight_t highlight;
  // state set by buttons
  dt_lib_histogram_scope_type_t scope_type;
  dt_lib_histogram_scale_t histogram_scale;
  dt_lib_histogram_waveform_type_t waveform_type;
  dt_lib_histogram_vectorscope_type_t vectorscope_type;
  double vectorscope_angle;
  gboolean red, green, blue;
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("histogram");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_histogram_process_histogram(dt_lib_histogram_t *const d, const float *const input,
                                             const dt_histogram_roi_t *const roi)
{
  dt_dev_histogram_collection_params_t histogram_params = { 0 };
  const dt_iop_colorspace_type_t cst = iop_cs_rgb;
  dt_dev_histogram_stats_t histogram_stats = { .bins_count = HISTOGRAM_BINS, .ch = 4, .pixels = 0 };
  uint32_t histogram_max[4] = { 0 };

  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  d->histogram_max = 0;
  memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);

  histogram_params.roi = roi;
  histogram_params.bins_count = HISTOGRAM_BINS;
  histogram_params.mul = histogram_params.bins_count - 1;

  // FIXME: set up "custom" histogram worker which can do colorspace conversion on fly -- in cases that we need to do that -- may need to add from colorspace to dt_dev_histogram_collection_params_t
  dt_histogram_helper(&histogram_params, &histogram_stats, cst, iop_cs_NONE, input, &d->histogram, FALSE, NULL);
  dt_histogram_max_helper(&histogram_stats, cst, iop_cs_NONE, &d->histogram, histogram_max);
  d->histogram_max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "final histogram took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

static void _lib_histogram_process_waveform(dt_lib_histogram_t *const d, const float *const input,
                                            const dt_histogram_roi_t *const roi)
{
  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  const int sample_width = MAX(1, roi->width - roi->crop_width - roi->crop_x);
  const int sample_height = MAX(1, roi->height - roi->crop_height - roi->crop_y);

  // Note that, with current constants, the input buffer is from the
  // preview pixelpipe and should be <= 1440x900x4. The output buffer
  // will be <= 360x175x4. Hence process works with a relatively small
  // quantity of data.
  const float *const restrict in = DT_IS_ALIGNED((const float *const restrict)input);
  float *const restrict wf_linear = DT_IS_ALIGNED((float *const restrict)d->waveform_linear);

  // Use integral sized bins for columns, as otherwise they will be
  // unequal and have banding. Rely on draw to smoothly do horizontal
  // scaling. For a 3:2 image, "landscape" orientation, bin_width will
  // generally be 4, for "portrait" it will generally be 3.
  // Note that waveform_stride is pre-initialized/hardcoded,
  // but waveform_width varies, depending on preview image
  // width and # of bins.
  const size_t bin_width = ceilf(sample_width / (float)d->waveform_max_width);
  const size_t wf_width = ceilf(sample_width / (float)bin_width);
  d->waveform_width = wf_width;

  dt_iop_image_fill(wf_linear, 0.0f, wf_width, d->waveform_height, 4);

  // Every bin_width x height portion of the image is being described
  // in a 1 pixel x waveform_height portion of the histogram.
  // NOTE: if constant is decreased, will brighten output
  const float brightness = d->waveform_height / 40.0f;
  const float scale = brightness / (sample_height * bin_width);

  // 1.0 is at 8/9 of the height!
  const size_t height_i = d->waveform_height-1;
  const float height_f = height_i;

  // count the colors
  // FIXME: could flip x/y axes here and when reading to make row-wise iteration?
  // FIXME: Try histogram-style worker threads to process by row and consolidate results. Have the workers do colorspace conversion per-pixel. As there will be no intermediate buffer, even 20 per-thread output buffers will still use less memory.
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, wf_linear, roi, wf_width, bin_width, height_f, height_i, scale) \
  schedule(static)
#endif
  for(size_t out_x = 0; out_x < wf_width; out_x++)
  {
    const size_t x_from = out_x * bin_width + roi->crop_x;
    const size_t x_high = MIN(x_from + bin_width, roi->width - roi->crop_width);
    for(size_t in_x = x_from; in_x < x_high; in_x++)
    {
      for(size_t in_y = roi->crop_y; in_y < roi->height - roi->crop_height; in_y++)
      {
        // While it would be nice to use for_each_channel(), making
        // the BGR/RGB flip doesn't allow for this. Regardless, the
        // fourth channel will be ignored when waveform is drawn.
        for(size_t k = 0; k < 3; k++)
        {
          const float v = 1.0f - (8.0f / 9.0f) * in[4U * (roi->width * in_y + in_x) + (2U - k)];
          const size_t out_y = isnan(v) ? 0 : MIN((size_t)fmaxf(v*height_f, 0.0f), height_i);
          wf_linear[4U * (wf_width * out_y + out_x) + k] += scale;
        }
      }
    }
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "final histogram waveform took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(rgb, chromaticity:16)
#endif
static inline void rgb_to_chromaticity(const float rgb[4],
                                       float chromaticity[4],
                                       dt_iop_order_iccprofile_info_t *prof,
                                       dt_lib_histogram_vectorscope_type_t cs)
{
  float XYZ_D50[4] DT_ALIGNED_PIXEL;
  // NOTE: see for comparison/reference rgb_to_JzCzhz() in color_picker.c
  // this goes to the PCS which has standard illuminant D50
  dt_ioppr_rgb_matrix_to_xyz(rgb, XYZ_D50, prof->matrix_in, prof->lut_in,
                             prof->unbounded_coeffs_in, prof->lutsize,
                             prof->nonlinearlut);
  if(cs == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV)
  {
    // FIXME: do have to worry about chromatic adaptation? this assumes that the white point is D50 -- if we have a D65 whitepoint profile, should we instead adapt to D65 then convert to L*u*v* with a D65 whitepoint?
    float xyY_D50[4] DT_ALIGNED_PIXEL;
    dt_XYZ_to_xyY(XYZ_D50, xyY_D50);
    // FIXME: this is u*v* (D50 corrected), more helpful than u'v', yes?
    dt_xyY_to_Luv(xyY_D50, chromaticity);
  }
  else
  {
    // FIXME: can skip a hop by pre-multipying matrices: see colorbalancergb and dt_develop_blendif_init_masking_profile() for how to make hacked profile
    float XYZ_D65[4] DT_ALIGNED_PIXEL;
    // If the profile whitepoint is D65, its RGB -> XYZ conversion
    // matrix has been adapted to D50 (PCS standard) via
    // Bradford. Hence using Bradford againt o adapt back to D65 gives
    // a pretty clean reversal (to approx. 4 significant digits) of
    // the transform.
    // FIXME: if the profile whitepoint is D50 (ProPhoto...), then should we use a nicer adaptation (CAT16?) to D65?
    /*
    // CAT16
    const float M[3][4] DT_ALIGNED_ARRAY = {
      {  9.80760485e-01,  -4.25541784e-17,  -7.61959005e-19},
      {  4.82934624e-17,   1.01555271e+00,  -7.63213113e-18},
      { -6.47162968e-19,  -5.69389701e-19,   1.30191586e+00}};
    for(size_t x = 0; x < 3; x++)
      XYZ_D65[x] = M[x][0] * XYZ_D50[0] + M[x][1] * XYZ_D50[1] + M[x][2] * XYZ_D50[2];
    */
    dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
    dt_XYZ_2_JzAzBz(XYZ_D65, chromaticity);
  }
}

static void _lib_histogram_process_vectorscope(dt_lib_histogram_t *d, const float *const input, int width, int height)
{
  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  const int vs_diameter = d->vectorscope_diameter;
  const int vs_alpha_stride = d->vectorscope_alpha_stride;
  uint8_t *const vs_alpha = d->vectorscope_alpha;
  const dt_lib_histogram_vectorscope_type_t vs_type = d->vectorscope_type;

  // FIXME: is this available from caller?
  // FIXME: why convert in caller to histogram profile if we're going to convert again here? instead just use the matrix of whatever the input is to dt_lib_histogram_process(), as in theory the conversion XYZ -> histogram_matrix_out -> RGB -> histogram_matrix_in -> XYZ is a noop, can just do pipeline RGB -> XYZ
  dt_iop_order_iccprofile_info_t *histogram_profile = dt_ioppr_get_histogram_profile_info(darktable.develop);
  if(!histogram_profile) return;
  // FIXME: as in colorbalancergb, repack matrix for SEE?

  // get profile primaries/secondaries in JzAzBz
  // there's no guarantee that there is a chromaticity tag in the
  // profile, so simply feed RGB colors through profile to PCS then
  // JzAzBz
  // FIXME: render the gamut triangle -- presumably in xyY so would need to render some points and interpolate -- at which point just build another image buffer for the overlay?
  float in_rgb[6][4] DT_ALIGNED_PIXEL = { {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f},
                                          {0.f, 1.f, 1.f, 1.f}, {1.f, 0.f, 1.f, 1.f}, {1.f, 1.f, 0.f, 1.f} };
  float max_diam = 0.0f;
  for(int k=0; k<6; k++)
  {
    // FIXME: hacky store of coordinates in 2nd/3rd elements of array
    float chromaticity[4] DT_ALIGNED_PIXEL;
    rgb_to_chromaticity(in_rgb[k], chromaticity, histogram_profile, vs_type);
    max_diam = MAX(max_diam, hypotf(chromaticity[1], chromaticity[2]));
    d->vectorscope_graticule[k][0] = chromaticity[1];
    d->vectorscope_graticule[k][1] = chromaticity[2];
  }
  // scale graticule chromaticity to display
  // FIXME: should really center top/bottom points rather than whitepoint?
  for(int k=0; k<6; k++)
  {
    d->vectorscope_graticule[k][0] /= max_diam;
    d->vectorscope_graticule[k][1] /= max_diam;
  }

  // FIXME: pre-allocate?
  float *const restrict binned = dt_iop_image_alloc(vs_diameter, vs_diameter, 1);
  dt_iop_image_fill(binned, 0.0f, vs_diameter, vs_diameter, 1);
  // FIXME: faster to have bins just record count and multiply by scale after?
  const float scale = 4.0f * (vs_diameter * vs_diameter) / (width * height * 255.0f);

  const float *const restrict in = DT_IS_ALIGNED((const float *const restrict)input);
  const size_t nfloats = 4 * width * height;

  // count into bins
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, binned, nfloats, histogram_profile, vs_diameter, max_diam, scale, vs_type) \
  schedule(static)
#endif
  for(size_t k = 0; k < nfloats; k += 4)
  {
    // Are there are unnecessary color math hops? Right now the data
    // comes into dt_lib_histogram_process() in a known profile
    // (usually from pixelpipe). Then (usually) it gets converted to
    // the histogram profile. Here it gets converted to XYZ D50 before
    // making its way to L*u*v* or JzAzBz:
    //   RGB (pixelpipe) -> XYZ(PCS, D50) -> RGB (histogram) -> XYZ (PCS, D50) -> chromaticity
    // Given that the histogram profile is "well behaved" and the
    // conversion to histogram profile is relative colorimetric, how
    // does this compare to:
    //   RGB (pixelpipe) -> XYZ(PCS, D50) -> chromaticity
    float chromaticity[4] DT_ALIGNED_PIXEL;
    const float *const restrict pix_in = __builtin_assume_aligned(in + k, 16);
    rgb_to_chromaticity(pix_in, chromaticity, histogram_profile, vs_type);
    const int out_x = vs_diameter * (chromaticity[1] / max_diam + 0.5f);
    const int out_y = vs_diameter * (chromaticity[2] / max_diam + 0.5f);

    // clip (not clamp) any out-of-scale values, so there aren't light edges
    // FIXME: should the output buffer be the dimensions of the current drawable widget -- rather than square -- so it doesn't lose data to the sides?
    if(out_x >= 0 && out_x < vs_diameter-1 && out_y >= 0 && out_y <= vs_diameter-1)
    {
      float *const restrict bin_out = binned + out_y * vs_diameter + out_x;
      // FIXME: make a "false color" variant view
      *bin_out += scale;
    }
  }

  // FIXME: do same gamma trick here as in waveform
  // FIXME: make the gamma code local to simplify, just need interpolation function
  const float gamma = 1.0f / 1.5f;

  // loop appears to be too small to benefit w/OpenMP
  for(size_t out_y = 0; out_y < vs_diameter; out_y++)
    for(int out_x = 0; out_x < vs_diameter; out_x++)
    {
      const float *const restrict bin_in = binned + out_y * vs_diameter + out_x;
      uint8_t *const restrict alpha_out = vs_alpha + out_y * vs_alpha_stride + out_x;
      // FIXME: use cache or linear interpolate from pre-calculated
      *alpha_out = CLAMP(powf(*bin_in, gamma) * 255.0f, 0, 255);
    }

  dt_free_align(binned);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "final vectorscope took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

static void dt_lib_histogram_process(struct dt_lib_module_t *self, const float *const input,
                                     int width, int height,
                                     dt_colorspaces_color_profile_type_t in_profile_type, const gchar *in_profile_filename)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  float *img_display = NULL;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);
    d->waveform_width = 0;
    d->vectorscope_graticule[0][0] = NAN;
    dt_pthread_mutex_unlock(&d->lock);
    return;
  }

  // FIXME: scope goes black when click histogram lib colorpicker on -- is this meant to happen?
  // FIXME: scope doesn't redraw when click histogram lib colorpicker off -- is this meant to happen?
  dt_histogram_roi_t roi = { .width = width, .height = height,
                             .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0 };

  // Constraining the area if the colorpicker is active in area mode
  // FIXME: only need to do colorspace conversion below on roi
  // FIXME: if the only time we use roi in histogram to limit area is here, and whenever we use tether there is no colorpicker (true?), and if we're always doing a colorspace transform in darkroom and clip to roi during conversion, then can get rid of all roi code for common/histogram?
  // when darkroom colorpicker is active, gui_module is set to colorout
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM &&
     dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
     && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    if(darktable.lib->proxy.colorpicker.size == DT_COLORPICKER_SIZE_BOX)
    {
      roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_box[0] * width));
      roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_box[1] * height));
      roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_box[2] * width));
      roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_box[3] * height));
    }
    else
    {
      roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
      roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
    }
  }

  // Convert pixelpipe output to histogram profile. If in tether view,
  // then the image is already converted by the caller.
  // FIXME: do conversion in-place in the processing to save an extra buffer? -- at least for waveform, which already has to touch each pixel -- will need logic from _transform_matrix_rgb() -- or better yet a per-pixel callback within _transform_matrix_rgb()-ish code
  // FIXME: in case of vectorscope, it needs XYZ data, so skip this conversion and instead it's enough that it has input & in_profile_type -- though then we don't see the result of a relative colorimetric conversion to the histogram profile...
  if(in_profile_type != DT_COLORSPACE_NONE)
  {
    const dt_iop_order_iccprofile_info_t *const profile_info_from
      = dt_ioppr_add_profile_info_to_list(dev, in_profile_type, in_profile_filename, INTENT_PERCEPTUAL);

    dt_colorspaces_color_profile_type_t out_profile_type;
    const char *out_profile_filename;
    dt_ioppr_get_histogram_profile_type(&out_profile_type, &out_profile_filename);
    if(out_profile_type != DT_COLORSPACE_NONE)
    {
      const dt_iop_order_iccprofile_info_t *const profile_info_to =
        dt_ioppr_add_profile_info_to_list(dev, out_profile_type, out_profile_filename, DT_INTENT_RELATIVE_COLORIMETRIC);
      img_display = dt_alloc_align_float((size_t)4 * width * height);
      if(!img_display) return;
      dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height, profile_info_from,
                                              profile_info_to, "final histogram");
    }
  }

  dt_pthread_mutex_lock(&d->lock);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      _lib_histogram_process_histogram(d, img_display ? img_display : input, &roi);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      _lib_histogram_process_waveform(d, img_display ? img_display : input, &roi);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      // FIXME: only work on roi
      _lib_histogram_process_vectorscope(d, img_display ? img_display : input, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
      g_assert_not_reached();
  }
  dt_pthread_mutex_unlock(&d->lock);

  if(img_display)
    dt_free_align(img_display);
}


static void _lib_histogram_draw_histogram(dt_lib_histogram_t *d, cairo_t *cr,
                                          int width, int height, const uint8_t mask[3])
{
  if(!d->histogram_max) return;
  const float hist_max = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR ? d->histogram_max
                                                                       : logf(1.0 + d->histogram_max);
  // The alpha of each histogram channel is 1, hence the primaries and
  // overlaid secondaries and neutral colors should be about the same
  // brightness. The combined group is then drawn with an alpha, which
  // dims things down.
  cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
  cairo_translate(cr, 0, height);
  cairo_scale(cr, width / 255.0, -(height - 10) / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      set_color(cr, darktable.bauhaus->graph_colors[k]);
      dt_draw_histogram_8(cr, d->histogram, 4, k, d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR);
    }
  cairo_pop_group_to_source(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_paint_with_alpha(cr, 0.5);
}

static void _lib_histogram_draw_waveform_channel(dt_lib_histogram_t *d, cairo_t *cr, int ch)
{
  // map linear waveform data to a display colorspace
  const float *const restrict wf_linear = DT_IS_ALIGNED((const float *const restrict)d->waveform_linear);
  float *const restrict wf_display = DT_IS_ALIGNED((float *const restrict)d->waveform_display);
  const int wf_width = d->waveform_width;
  const int wf_height = d->waveform_height;
  // colors used to represent primary colors
  // FIXME: force a redraw when colors have changed via user entering new CSS in preferences -- is there a signal for this?
  const GdkRGBA *const css_primaries = darktable.bauhaus->graph_colors;
  const float DT_ALIGNED_ARRAY primaries_linear[3][4] = {
    {css_primaries[2].blue, css_primaries[2].green, css_primaries[2].red, 1.0f},
    {css_primaries[1].blue, css_primaries[1].green, css_primaries[1].red, 1.0f},
    {css_primaries[0].blue, css_primaries[0].green, css_primaries[0].red, 1.0f},
  };
  const size_t nfloats = 4U * wf_width * wf_height;
  // this should be <= 250K iterations, hence not worth the overhead to thread
  for(size_t p = 0; p < nfloats; p += 4)
  {
    const float src = MIN(1.0f, wf_linear[p + ch]);
    for_four_channels(k,aligned(wf_display,primaries_linear:64))
    {
      wf_display[p+k] = src * primaries_linear[ch][k];
    }
  }

  // shortcut for a fast gamma change
  const dt_iop_order_iccprofile_info_t *profile_linear =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *profile_work =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL);
  // in place transform will preserve alpha
  // dt's transform is approx. 2x faster than LCMS here
  // FIXME: optimize by just doing the extrapolate_lut() work and make our own sensible LUT with enough resolution to provide continuous tone
  dt_ioppr_transform_image_colorspace_rgb(wf_display, wf_display, wf_width, wf_height,
                                          profile_linear, profile_work, "waveform gamma");

  const size_t wf_width_floats = 4U * wf_width;
  uint8_t *const restrict wf_8bit = DT_IS_ALIGNED((uint8_t *const restrict)d->waveform_8bit);
  const size_t wf_8bit_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, wf_width);
  // not enough iterations to be worth threading
  for(size_t y = 0; y < wf_height; y++)
  {
#ifdef _OPENMP
#pragma simd aligned(wf_display, wf_8bit : 64)
#endif
    for(size_t k = 0; k < wf_width_floats; k++)
    {
      // linear -> display transform can return pixels > 1, hence limit these
      wf_8bit[y * wf_8bit_stride + k] = MIN(255, (int)(wf_display[y * wf_width_floats + k] * 255.0f));
    }
  }
  // FIXME: everything up to here should be invariant (unless CSS changes) so put it in process rather than draw

  cairo_surface_t *source
    = dt_cairo_image_surface_create_for_data(wf_8bit, CAIRO_FORMAT_ARGB32,
                                             wf_width, wf_height, wf_8bit_stride);
  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint_with_alpha(cr, 0.5);
  cairo_surface_destroy(source);
}

static void _lib_histogram_draw_waveform(dt_lib_histogram_t *d, cairo_t *cr,
                                         int width, int height,
                                         const uint8_t mask[3])
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_scale(cr, darktable.gui->ppd*width/d->waveform_width,
              darktable.gui->ppd*height/d->waveform_height);

  for(int ch = 0; ch < 3; ch++)
    if(mask[2-ch])
      _lib_histogram_draw_waveform_channel(d, cr, ch);
  cairo_restore(cr);
}

static void _lib_histogram_draw_rgb_parade(dt_lib_histogram_t *d, cairo_t *cr, int width, int height)
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_scale(cr, darktable.gui->ppd*width/(d->waveform_width*3),
              darktable.gui->ppd*height/d->waveform_height);
  for(int ch = 2; ch >= 0; ch--)
  {
    _lib_histogram_draw_waveform_channel(d, cr, ch);
    cairo_translate(cr, d->waveform_width/darktable.gui->ppd, 0);
  }
  cairo_restore(cr);
}

static void _lib_histogram_draw_vectorscope(dt_lib_histogram_t *d, cairo_t *cr,
                                            int width, int height)
{
  const int vs_diameter = d->vectorscope_diameter;
  const int min_size = MIN(width, height);

  // FIXME: draw a gray background behind the vectorscope square, the left/right dark with some data there (primaries, whitepoint, etc.)

  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_translate(cr, width / 2., height / 2.);
  cairo_rotate(cr, d->vectorscope_angle);

  // traditional video editor's vectorscope is oriented with x-axis Y
  // -> B, y-axis C -> R but CIE 1976 UCS is graphed x-axis as u (G ->
  // M), y-axis as v (B -> Y), so do that and keep to the proper color
  // math
  cairo_scale(cr, 1., -1.);

  // graticule: histogram profile primaries/secondaries
  // FIXME: also add dots for input/work/output profiles
  // FIXME: add gamut bounding lines to connect the dots (which need to be plotted in XYZ?)
  // from Sobotka:
  // 1. The input encoding primaries. How dd the image start out life? What is valid data within that? What is invalid introduced by error of camera virtual primaries solving or math such as resampling an image such that negative lobes result?
  // 2. The working reference primaries. How did 1. end up in 2.? Are there negative and therefore nonsensical values in the working space? Should a gamut mapping pass be applied before work, between 1. and 2.?
  // 3. The output primaries rendition. From a selection of gamut mappings, is one required between 2. and 3.?"
  const GdkRGBA *const colors = darktable.bauhaus->graph_colors;
  for(int k=0; k<6; k++)
  {
    // FIXME: if/when have a color vectorscope, make these dots gray to complement it
    cairo_set_source_rgba(cr, colors[k].red, colors[k].green, colors[k].blue, colors[k].alpha * (k<3 ? 0.7 : 0.5));
    cairo_arc(cr, d->vectorscope_graticule[k][0] * min_size * 0.5,
              d->vectorscope_graticule[k][1] * min_size * 0.5, min_size/(k<3 ? 40.0 : 60.0), 0., M_PI * 2.);
    cairo_fill(cr);
  }

  // the vectorscope graph itself
  // FIXME: the vectorscope hard-clips on the left/right for huge #'s -- instead could catch configure event, size buffers to aspect ratio, and then never clip -- or simply in calc decrease diameter by ~ 25% then increase correspondingly here, so less likely to clip -- and/or draw some sort of gradient mask over the edges so that they fade out

  cairo_translate(cr, min_size * -0.5, min_size * -0.5);
  // FIXME: use ppd? if not, cast min_size to double
  cairo_scale(cr, darktable.gui->ppd*min_size/vs_diameter, darktable.gui->ppd*min_size/vs_diameter);

  cairo_set_source_rgba(cr, 1., 1., 1., 0.7);
  cairo_surface_t *alpha
    = dt_cairo_image_surface_create_for_data(d->vectorscope_alpha, CAIRO_FORMAT_A8,
                                             vs_diameter, vs_diameter, d->vectorscope_alpha_stride);
  // FIXME: what happens if mask has color (not just alpha) -- can use this to calculate false color vectorscope and draw it as a pattern or use it as a mask to draw it white? and if so could also simplify waveform drawing code above, to use color mask to alter channel visibility
  cairo_mask_surface(cr, alpha, 0.0, 0.0);
  cairo_surface_destroy(alpha);
  cairo_restore(cr);
}

static void _draw_vectorscope_lines(cairo_t *cr, int width, int height)
{
  const double min_size = MIN(width, height);
  // FIXME: need DT_PIXEL_APPLY_DPI()?
  const double w_ctr = min_size / 25.0f;

  cairo_save(cr);
  cairo_translate(cr, width/2., height/2.);

  // FIXME: a polar coordinate style grid would be interesting/helpful for, as currently the scaling changes based on histogram profile, and this would give some sort of reference

  // central crosshair
  set_color(cr, darktable.bauhaus->graph_overlay);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  dt_draw_line(cr, -w_ctr, 0.0f, w_ctr, 0.0f);
  cairo_stroke(cr);
  dt_draw_line(cr, 0.0f, -w_ctr, 0.0f, w_ctr);
  cairo_stroke(cr);

  cairo_restore(cr);
}

// FIXME: have different drawable for each scope in a stack -- simplifies this function from being a swath of conditionals -- then esentially draw callbacks _lib_histogram_draw_waveform, _lib_histogram_draw_rgb_parade, etc.
// FIXME: if exposure change regions are separate widgets, then we could have a menu to swap in different overlay widgets (sort of like basic adjustments) to adjust other things about the image, e.g. tone equalizer, color balance, etc.
static gboolean _drawable_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width, height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.5)); // borders width

  // Draw frame and background
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_stroke_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);
  cairo_restore(cr);

  // exposure change regions
  if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 7.0/9.0 * height, width, height);
    else
      cairo_rectangle(cr, 0, 0, 0.2 * width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 0, width, 7.0/9.0 * height);
    else
      cairo_rectangle(cr, 0.2 * width, 0, width, height);
    cairo_fill(cr);
  }

  // draw grid
  set_color(cr, darktable.bauhaus->graph_grid);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      dt_draw_grid(cr, 4, 0, 0, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      dt_draw_waveform_lines(cr, 0, 0, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      _draw_vectorscope_lines(cr, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
      g_assert_not_reached();
  }

  // FIXME: should set histogram buffer to black if have just entered tether view and nothing is displayed
  dt_pthread_mutex_lock(&d->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_TETHERING || dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    const uint8_t mask[3] = { d->red, d->green, d->blue };
    switch(d->scope_type)
    {
      case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
        _lib_histogram_draw_histogram(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
        if(!d->waveform_width) break;
        if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
          _lib_histogram_draw_waveform(d, cr, width, height, mask);
        else
          _lib_histogram_draw_rgb_parade(d, cr, width, height);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
        if(!isnan(d->vectorscope_graticule[0][0]))
          _lib_histogram_draw_vectorscope(d, cr, width, height);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
        g_assert_not_reached();
    }
  }
  dt_pthread_mutex_unlock(&d->lock);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "scope draw took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }

  return TRUE;
}

static gboolean _drawable_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                 gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  if(d->dragging)
  {
    // FIXME: dragging the vectorscope should change white balance -- or perhaps its scale
    const float diff = d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? d->button_down_y - event->y
                                                                        : event->x - d->button_down_x;
    const int range = d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? allocation.height
                                                                       : allocation.width;
    // FIXME: see dt_bauhaus_slider_postponed_value_change(): delay processing until the pixelpipe can update based on dev->preview_average_delay for smoother interaction
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      const float exposure = d->button_down_value + diff * 4.0f / (float)range;
      dt_dev_exposure_set_exposure(dev, exposure);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      const float black = d->button_down_value - diff * .1f / (float)range;
      dt_dev_exposure_set_black(dev, black);
    }
  }
  else
  {
    const float x = event->x;
    const float y = event->y;
    const float posx = x / (float)(allocation.width);
    const float posy = y / (float)(allocation.height);
    const dt_lib_histogram_highlight_t prior_highlight = d->highlight;
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
    const gboolean hooks_available = (cv->view(cv) == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(dev);

    // FIXME: make just one tooltip for the widget depending on whether it is draggable or not, and set it when enter the view
    if(!hooks_available || d->scope_type == DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
      gtk_widget_set_tooltip_text(widget, _("ctrl+scroll to change display height"));
    }
    // FIXME: could a GtkRange be used to do this work?
    else if((posx < 0.2f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM) ||
            (posy > 7.0f/9.0f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM))
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT;
      gtk_widget_set_tooltip_text(widget, _("drag to change black point,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    else
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE;
      gtk_widget_set_tooltip_text(widget, _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    if(prior_highlight != d->highlight)
    {
      dt_control_queue_redraw_widget(widget);
      if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
      {
        // FIXME: should really use named cursors, and differentiate between "grab" and "grabbing"
        dt_control_change_cursor(GDK_HAND1);
      }
    }
  }

  //bubble event to eventbox to update the button tooltip
  return FALSE;
}

static gboolean _drawable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;

  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    if(event->type == GDK_2BUTTON_PRESS)
    {
      dt_dev_exposure_reset_defaults(dev);
    }
    else
    {
      // FIXME: should change cursor from "grab" to "grabbing", but this would mean rewriting dt_control_change_cursor() to use gdk_cursor_new_from_name()
      d->dragging = TRUE;
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        d->button_down_value = dt_dev_exposure_get_exposure(dev);
      else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
        d->button_down_value = dt_dev_exposure_get_black(dev);
      d->button_down_x = event->x;
      d->button_down_y = event->y;
    }
  }

  return TRUE;
}

static gboolean _drawable_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    // bubble to adjusting the overall widget size
    return FALSE;
  }
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  int delta_y;
  // note are using unit rather than smooth scroll events, as
  // exposure changes can get laggy if handling a multitude of smooth
  // scroll events
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    dt_develop_t *dev = darktable.develop;
    if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
    {
      // FIXME: see dt_bauhaus_slider_postponed_value_change(): delay processing until the pixelpipe can update based on dev->preview_average_delay for smoother interaction
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
      {
        const float ce = dt_dev_exposure_get_exposure(dev);
        dt_dev_exposure_set_exposure(dev, ce - 0.15f * delta_y);
      }
      else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
      {
        const float cb = dt_dev_exposure_get_black(dev);
        dt_dev_exposure_set_black(dev, cb + 0.001f * delta_y);
      }
    }
  }

  return TRUE;
}

static gboolean _drawable_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                  gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  d->dragging = FALSE;
  // hack to recalculate the highlight as mouse may be over a different part of the widget
  // FIXME: generate an event instead?
  _drawable_motion_notify_callback(widget, (GdkEventMotion *)event, user_data);
  return TRUE;
}

static gboolean _drawable_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  // if dragging, gtk keeps up motion notifications until mouse button
  // is released, at which point we'll get another leave event for
  // drawable if pointer is still outside of the widget
  if(!d->dragging && d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    dt_control_queue_redraw_widget(widget);
  }
  // event should bubble up to the eventbox
  return FALSE;
}

static void _histogram_scale_update(const dt_lib_histogram_t *d)
{
  switch(d->histogram_scale)
  {
    case DT_LIB_HISTOGRAM_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_LINEAR:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
      g_assert_not_reached();
  }
  // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;
}

static void _waveform_view_update(const dt_lib_histogram_t *d)
{
  // FIXME: add other waveform types -- RGB parade overlaid top-to-bottom rather than left to right, possibly waveform calculated sideways (another button?)
  switch(d->waveform_type)
  {
    case DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set view to RGB parade"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_waveform_overlaid, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, TRUE);
      gtk_widget_set_sensitive(d->green_channel_button, TRUE);
      gtk_widget_set_sensitive(d->blue_channel_button, TRUE);
      break;
    case DT_LIB_HISTOGRAM_WAVEFORM_PARADE:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set view to waveform"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_rgb_parade, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, FALSE);
      gtk_widget_set_sensitive(d->green_channel_button, FALSE);
      gtk_widget_set_sensitive(d->blue_channel_button, FALSE);
      break;
    case DT_LIB_HISTOGRAM_WAVEFORM_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
      g_assert_not_reached();
  }
}

static void _vectorscope_view_update(dt_lib_histogram_t *d)
{
  // FIXME: add a "false color" variant, probably taking over the "red channel" button for this
  switch(d->vectorscope_type)
  {
    case DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set view to AzBz"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_luv, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_VECTORSCOPE_JZAZBZ:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set view to u*v*"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_jzazbz, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_VECTORSCOPE_N:
      g_assert_not_reached();
  }

  // generate data for changed view and trigger widget redraw
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv)  // this may be called by _scope_type_update() on init, before in a view
  {
    // redraw empty scope for immediate visual feedback
    d->vectorscope_graticule[0][0] = NAN;
    dt_control_queue_redraw_widget(d->scope_draw);

    if(cv->view(cv) == DT_VIEW_DARKROOM)
      dt_dev_process_preview(darktable.develop);
    else
      dt_control_queue_redraw_center();
  }
}

static void _scope_type_update(dt_lib_histogram_t *d)
{
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      gtk_widget_set_tooltip_text(d->scope_type_button, _("set mode to waveform"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_type_button),
                             dtgtk_cairo_paint_histogram_scope, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, TRUE);
      gtk_widget_set_sensitive(d->green_channel_button, TRUE);
      gtk_widget_set_sensitive(d->blue_channel_button, TRUE);
      _histogram_scale_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      gtk_widget_set_tooltip_text(d->scope_type_button, _("set mode to vectorscope"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_type_button),
                             dtgtk_cairo_paint_waveform_scope, CPF_NONE, NULL);
      // handles setting RGB channel button sensitive state
      _waveform_view_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      gtk_widget_set_tooltip_text(d->scope_type_button, _("set mode to histogram"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_type_button),
                             dtgtk_cairo_paint_vectorscope, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, FALSE);
      gtk_widget_set_sensitive(d->green_channel_button, FALSE);
      gtk_widget_set_sensitive(d->blue_channel_button, FALSE);
      _vectorscope_view_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      g_assert_not_reached();
  }
}

static void _scope_type_clicked(GtkWidget *button, dt_lib_histogram_t *d)
{
  // NOTE: this isn't a "real" button but more of a tri-state toggle button
  d->scope_type = (d->scope_type + 1) % DT_LIB_HISTOGRAM_SCOPE_N;
  dt_conf_set_string("plugins/darkroom/histogram/mode", dt_lib_histogram_scope_type_names[d->scope_type]);
  _scope_type_update(d);

  // redraw scope now, even if it isn't up to date, so that there is
  // immediate feedback on button press even though there will be a
  // lag to process the scope data
  dt_control_queue_redraw_widget(d->scope_draw);

  // generate data for changed scope and trigger widget redraw
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

static void _scope_view_clicked(GtkWidget *button, dt_lib_histogram_t *d)
{
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      d->histogram_scale = (d->histogram_scale + 1) % DT_LIB_HISTOGRAM_N;
      dt_conf_set_string("plugins/darkroom/histogram/histogram",
                         dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
      _histogram_scale_update(d);
      dt_control_queue_redraw_widget(d->scope_draw);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1) % DT_LIB_HISTOGRAM_WAVEFORM_N;
      dt_conf_set_string("plugins/darkroom/histogram/waveform",
                         dt_lib_histogram_waveform_type_names[d->waveform_type]);
      _waveform_view_update(d);
      dt_control_queue_redraw_widget(d->scope_draw);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      d->vectorscope_type = (d->vectorscope_type + 1) % DT_LIB_HISTOGRAM_VECTORSCOPE_N;
      dt_conf_set_string("plugins/darkroom/histogram/vectorscope",
                         dt_lib_histogram_vectorscope_type_names[d->vectorscope_type]);
      _vectorscope_view_update(d);
      dt_control_queue_redraw_widget(d->scope_draw);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
      g_assert_not_reached();
  }
}

// FIXME: these all could be the same function with different user_data
static void _red_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->red = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_tooltip_text(button, d->red ? _("click to hide red channel") : _("click to show red channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _green_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->green = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_tooltip_text(button, d->green ? _("click to hide green channel") : _("click to show green channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _blue_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->blue = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_tooltip_text(button, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static gboolean _eventbox_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                 gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  gtk_widget_show(d->button_box);
  return TRUE;
}

static gboolean _eventbox_motion_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                 gpointer user_data)
{
  //This is required in order to correctly display the button tooltips
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  gtk_widget_set_tooltip_text(d->green_channel_button, d->green ? _("click to hide green channel") : _("click to show green channel"));
  gtk_widget_set_tooltip_text(d->blue_channel_button, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  gtk_widget_set_tooltip_text(d->red_channel_button, d->red ? _("click to hide red channel") : _("click to show red channel"));
  _scope_type_update(d);
  return TRUE;
}

static gboolean _eventbox_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                 gpointer user_data)
{
  // when click between buttons on the buttonbox a leave event is generated -- ignore it
  if(!(event->mode == GDK_CROSSING_UNGRAB && event->detail == GDK_NOTIFY_INFERIOR))
  {
    dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
    gtk_widget_hide(d->button_box);
  }
  return TRUE;
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y) &&
     dt_modifier_is(event->state, GDK_CONTROL_MASK) && !darktable.gui->reset)
  {
    /* set size of navigation draw area */
    const float histheight = clamp_range_f(dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f + 10 * delta_y, 100.0f, 200.0f);
    dt_conf_set_int("plugins/darkroom/histogram/height", histheight);
    gtk_widget_set_size_request(widget, -1, DT_PIXEL_APPLY_DPI(histheight));
  }
  return TRUE;
}

static gboolean _lib_histogram_collapse_callback(GtkAccelGroup *accel_group,
                                                 GObject *acceleratable, guint keyval,
                                                 GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;

  // Get the state
  const gint visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);

  return TRUE;
}

static gboolean _lib_histogram_cycle_mode_callback(GtkAccelGroup *accel_group,
                                                   GObject *acceleratable, guint keyval,
                                                   GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  // The cycle order is Hist log -> lin -> waveform -> parade -> vectorscope (update logic on more scopes)
  // FIXME: When switch modes, there is currently a hack to turn off the highlight and turn the cursor back to pointer, as we don't know what/if the new highlight is going to be. Right solution would be to have a highlight update function which takes cursor x,y and is called either here or on pointer motion. Really right solution is probably separate widgets for the drag areas which generate enter/leave events.
  // FIXME: can simplify this code: for view then type increment & compare enum max
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      if(d->histogram_scale == DT_LIB_HISTOGRAM_LOGARITHMIC)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->dragging = FALSE;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        dt_conf_set_string("plugins/darkroom/histogram/waveform",
                           dt_lib_histogram_waveform_type_names[d->waveform_type]);
        _scope_type_clicked(d->scope_type_button, d);
        d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
        dt_control_change_cursor(GDK_LEFT_PTR);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->dragging = FALSE;
        d->vectorscope_type = DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV;
        dt_conf_set_string("plugins/darkroom/histogram/vectorscope",
                           dt_lib_histogram_vectorscope_type_names[d->vectorscope_type]);
        _scope_type_clicked(d->scope_type_button, d);
        d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
        dt_control_change_cursor(GDK_LEFT_PTR);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      if(d->vectorscope_type == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
        dt_conf_set_string("plugins/darkroom/histogram/histogram",
                           dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
        // don't need to cancel dragging or lose highlight so long as vectorscope isn't draggable
        _scope_type_clicked(d->scope_type_button, d);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      // FIXME: use dt_unreachable_codepath_with_desc()
      g_assert_not_reached();
  }

  return TRUE;
}

static gboolean _lib_histogram_change_mode_callback(GtkAccelGroup *accel_group,
                                                    GObject *acceleratable, guint keyval,
                                                    GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = FALSE;
  d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
  dt_control_change_cursor(GDK_LEFT_PTR);
  _scope_type_clicked(d->scope_type_button, d);
  return TRUE;
}

static gboolean _lib_histogram_change_type_callback(GtkAccelGroup *accel_group,
                                                    GObject *acceleratable, guint keyval,
                                                    GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  _scope_view_clicked(d->scope_view_button, d);
  return TRUE;
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  // FIXME: it would be nice if process() just queued a redraw if not in live view, but then our draw code would have to have some other way to assure that the histogram image is current besides checking the pixelpipe to see if it has processed the current image
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_control_queue_redraw_widget(d->scope_draw);
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_lib_histogram_preview_updated_callback), self);
  }
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(d->button_box);

  // FIXME: set histogram data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_lib_histogram_preview_updated_callback),
                               self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc0(sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  dt_pthread_mutex_init(&d->lock, NULL);

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  // FIXME: this is now very legacy <= c3.2? -- lose this as now that conf enforces options (yes?) this will never happen
  gchar *str = dt_conf_get_string("plugins/darkroom/histogram/mode");
  for(dt_lib_histogram_scope_type_t i=0; i<DT_LIB_HISTOGRAM_SCOPE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_scope_type_names[i]) == 0)
      d->scope_type = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/histogram");
  for(dt_lib_histogram_scale_t i=0; i<DT_LIB_HISTOGRAM_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_histogram_scale_names[i]) == 0)
      d->histogram_scale = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/waveform");
  for(dt_lib_histogram_waveform_type_t i=0; i<DT_LIB_HISTOGRAM_WAVEFORM_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_waveform_type_names[i]) == 0)
      d->waveform_type = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/vectorscope");
  for(dt_lib_histogram_vectorscope_type_t i=0; i<DT_LIB_HISTOGRAM_VECTORSCOPE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_vectorscope_type_names[i]) == 0)
      d->vectorscope_type = i;
  g_free(str);

  int a = dt_conf_get_int("plugins/darkroom/histogram/vectorscope/angle");
  d->vectorscope_angle = a * M_PI / 180.0;

  d->histogram = (uint32_t *)calloc(4 * HISTOGRAM_BINS, sizeof(uint32_t));
  d->histogram_max = 0;

  // Waveform buffer doesn't need to be coupled with the histogram
  // widget size. The waveform is almost always scaled when
  // drawn. Choose buffer dimensions which produces workable detail,
  // don't use too much CPU/memory, and allow reasonable gradations
  // of tone.

  // Don't use absurd amounts of memory, exceed width of DT_MIPMAP_F
  // (which will be darktable.mipmap_cache->max_width[DT_MIPMAP_F]*2
  // for mosaiced images), nor make it too slow to calculate
  // (regardless of ppd). Try to get enough detail for a (default)
  // 350px panel, possibly 2x that on hidpi.  The actual buffer
  // width will vary with integral binning of image.
  // FIXME: increasing waveform_max_width increases processing speed less than increasing waveform_height -- tune these better?
  d->waveform_max_width = darktable.mipmap_cache->max_width[DT_MIPMAP_F]/2;
  // initially no waveform to draw
  d->waveform_width = 0;
  // 175 rows is the default histogram widget height. It's OK if the
  // widget height changes from this, as the width will almost always
  // be scaled. 175 rows is reasonable CPU usage and represents plenty
  // of tonal gradation. 256 would match the # of bins in a regular
  // histogram.
  d->waveform_height  = 175;
  d->waveform_linear  = dt_iop_image_alloc(d->waveform_max_width, d->waveform_height, 4);
  d->waveform_display = dt_iop_image_alloc(d->waveform_max_width, d->waveform_height, 4);
  d->waveform_8bit    = dt_alloc_align(64, sizeof(uint8_t) * 4 * d->waveform_height * d->waveform_max_width);

  // FIXME: what is the appropriate resolution for this: balance memory use, processing speed, helpful resolution
  d->vectorscope_diameter = 256;
  d->vectorscope_alpha_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->vectorscope_diameter);
  d->vectorscope_alpha = dt_alloc_align(64, sizeof(uint8_t) * d->vectorscope_diameter * d->vectorscope_alpha_stride);
  // initially no vectorscope to draw
  d->vectorscope_graticule[0][0] = NAN;

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = dt_lib_histogram_process;
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;

  // create widgets
  GtkWidget *overlay = gtk_overlay_new();

  // shows the scope, scale, and has draggable areas
  d->scope_draw = gtk_drawing_area_new();
  gtk_widget_set_tooltip_text(d->scope_draw, _("ctrl+scroll to change display height"));

  // a row of control buttons
  d->button_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_button_box_set_layout(GTK_BUTTON_BOX(d->button_box), GTK_BUTTONBOX_EXPAND);
  gtk_widget_set_valign(d->button_box, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box, GTK_ALIGN_END);

  // FIXME: should histogram/waveform/vectorscope each be its own widget, and a GtkStack to switch between them?

  // First two buttons choose scope type and view of that scope (if
  // applicable). On click dt_lib_histogram_t data is updated,
  // icons/tooltips are updated, and button sensitivity is set as
  // needed.

  // FIXME: this could be a combobox to allow for more types and not to have to swap the icon on click
  // icons will be filled in by _scope_type_update()
  d->scope_type_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->scope_type_button, FALSE, FALSE, 0);
  d->scope_view_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->scope_view_button, FALSE, FALSE, 0);

  // red/green/blue channel on/off
  // these are toggle boxes with a meaningful active state, unlike the type/view buttons
  d->red_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(d->red_channel_button, "red-channel-button");
  gtk_widget_set_tooltip_text(d->red_channel_button, d->red ? _("click to hide red channel") : _("click to show red channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->red_channel_button), d->red);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->red_channel_button, FALSE, FALSE, 0);

  d->green_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(d->green_channel_button, "green-channel-button");
  gtk_widget_set_tooltip_text(d->green_channel_button, d->green ? _("click to hide green channel") : _("click to show green channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->green_channel_button), d->green);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->green_channel_button, FALSE, FALSE, 0);

  d->blue_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(d->blue_channel_button, "blue-channel-button");
  gtk_widget_set_tooltip_text(d->blue_channel_button, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->blue_channel_button), d->blue);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->blue_channel_button, FALSE, FALSE, 0);

  // will change sensitivity of channel buttons, hence must run after all buttons are declared
  _scope_type_update(d);

  // FIXME: add a brightness control (via GtkScaleButton?)

  // assemble the widgets

  // The main widget is an overlay which has no window, and hence
  // can't catch events. We need something on top to catch events to
  // show/hide the buttons. The drawable is below the buttons, and
  // hence won't catch motion events for the buttons, and gets a leave
  // event when the cursor moves over the buttons.

  // |----- EventBox -----|
  // |                    |
  // |  |-- Overlay  --|  |
  // |  |              |  |
  // |  |  ButtonBox   |  |
  // |  |              |  |
  // |  |--------------|  |
  // |  |              |  |
  // |  |  DrawingArea |  |
  // |  |              |  |
  // |  |--------------|  |
  // |                    |
  // |--------------------|

  GtkWidget *eventbox = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(overlay), d->scope_draw);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box);
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);
  self->widget = eventbox;

  gtk_widget_set_name(self->widget, "main-histogram");
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  /* connect callbacks */

  g_signal_connect(G_OBJECT(d->scope_type_button), "clicked", G_CALLBACK(_scope_type_clicked), d);
  g_signal_connect(G_OBJECT(d->scope_view_button), "clicked", G_CALLBACK(_scope_view_clicked), d);

  g_signal_connect(G_OBJECT(d->red_channel_button), "toggled", G_CALLBACK(_red_channel_toggle), d);
  g_signal_connect(G_OBJECT(d->green_channel_button), "toggled", G_CALLBACK(_green_channel_toggle), d);
  g_signal_connect(G_OBJECT(d->blue_channel_button), "toggled", G_CALLBACK(_blue_channel_toggle), d);

  gtk_widget_add_events(d->scope_draw, GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                       GDK_BUTTON_RELEASE_MASK | darktable.gui->scroll_mask);
  // FIXME: why does cursor motion over buttons trigger multiple draw callbacks?
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_drawable_draw_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "leave-notify-event",
                   G_CALLBACK(_drawable_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "button-press-event",
                   G_CALLBACK(_drawable_button_press_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "button-release-event",
                   G_CALLBACK(_drawable_button_release_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "motion-notify-event",
                   G_CALLBACK(_drawable_motion_notify_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "scroll-event",
                   G_CALLBACK(_drawable_scroll_callback), d);

  gtk_widget_add_events(eventbox, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(eventbox), "enter-notify-event",
                   G_CALLBACK(_eventbox_enter_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "leave-notify-event",
                   G_CALLBACK(_eventbox_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "motion-notify-event",
                   G_CALLBACK(_eventbox_motion_notify_callback), d);

  // handles scroll-to-resize behavior
  gtk_widget_add_events(self->widget, darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(self->widget), "scroll-event",
                   G_CALLBACK(_lib_histogram_scroll_callback), NULL);

  /* set size of histogram draw area */
  const float histheight = dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f;
  gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(histheight));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  free(d->histogram);
  dt_free_align(d->waveform_linear);
  dt_free_align(d->waveform_display);
  dt_free_align(d->waveform_8bit);
  dt_free_align(d->vectorscope_alpha);
  dt_pthread_mutex_destroy(&d->lock);

  g_free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/hide histogram"), GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "hide histogram"), GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/cycle histogram modes"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "cycle histogram modes"), 0, 0);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/switch histogram mode"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "switch histogram mode"), 0, 0);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/switch histogram type"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "switch histogram type"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/hide histogram",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_collapse_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "hide histogram",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_collapse_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/cycle histogram modes",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_cycle_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "cycle histogram modes",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_cycle_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/switch histogram mode",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "switch histogram mode",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/switch histogram type",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_type_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "switch histogram type",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_type_callback), self, NULL));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
