/*
 * Copyright © 2026  HarfBuzz contributors
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

/* Lets the file compile as C++ with a bare compiler command line that
 * does not pass -DHAVE_CONFIG_H; harmless under the Meson build. */
#ifndef HAVE_STDBOOL_H
#define HAVE_STDBOOL_H 1
#endif

#include "hb-test.h"
#include "hb-raster.h"

#include <hb-ot.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

/* Integration tests for shaping Thai text and rasterizing the shaped
 * outlines into one grayscale (A8) pixel grid.
 *
 * The interesting word is ก ่ ำ ข (U+0E01 KO KAI, U+0E48 MAI EK,
 * U+0E33 SARA AM, U+0E02 KHO KHAI).  The Thai shaper decomposes SARA AM
 * into NIKHAHIT (U+0E4D, the upper dot) + SARA AA (U+0E32, the right
 * vowel) and moves the dot back across above-base marks, so the tone and
 * the dot end up sharing the space above the base while SARA AA sets the
 * next pen.  See test/shape/data/in-house/tests/sara-am.tests and
 * src/hb-ot-shaper-thai.cc.
 *
 * Every font here is built in memory out of rectangle outlines with
 * deliberately distinct locations, so a selected outline has an
 * unambiguous pixel signature.  Coordinates are designed in pixels at
 * scale 100 with 1000 units-per-em: hb_font_set_scale (100, 100) makes
 * one design unit 0.1 raster units.  Pen positions below are therefore
 * all multiples of ten design pixels (base advance 500 -> 50 pixels).
 */

/* ── Tiny rectangle-outline TrueType font builder ──────────────────── */

struct rect_t { int x0, y0, x1, y1; };
struct glyph_t { std::vector<rect_t> rects; int advance = 500; };

static void
put16 (std::vector<uint8_t> &o, unsigned i, uint16_t v)
{ o[i] = v >> 8; o[i + 1] = v; }
static void
put32 (std::vector<uint8_t> &o, unsigned i, uint32_t v)
{ o[i] = v >> 24; o[i + 1] = v >> 16; o[i + 2] = v >> 8; o[i + 3] = v; }
static void
add16 (std::vector<uint8_t> &o, uint16_t v)
{ o.push_back (v >> 8); o.push_back (v); }
static void
add32 (std::vector<uint8_t> &o, uint32_t v)
{ o.push_back (v >> 24); o.push_back (v >> 16); o.push_back (v >> 8); o.push_back (v); }

/* Minimal GSUB: Thai script, ccmp feature, one SingleSubst that maps
 * glyph 3 to glyph 3 + @delta (the "OpenType-selected" tone). */
static std::vector<uint8_t>
make_thai_gsub (int16_t delta)
{
  std::vector<uint8_t> t (68, 0);
  put16 (t, 0, 1); put16 (t, 4, 10); put16 (t, 6, 30); put16 (t, 8, 44);
  put16 (t, 10, 1);
  t[12] = 't'; t[13] = 'h'; t[14] = 'a'; t[15] = 'i';
  put16 (t, 16, 8);
  put16 (t, 18, 4); put16 (t, 20, 0);
  put16 (t, 22, 0); put16 (t, 24, 0xFFFF); put16 (t, 26, 1); put16 (t, 28, 0);
  put16 (t, 30, 1); /* featureCount */
  t[32] = 'c'; t[33] = 'c'; t[34] = 'm'; t[35] = 'p';
  put16 (t, 36, 8);
  put16 (t, 38, 0); put16 (t, 40, 1); put16 (t, 42, 0);
  put16 (t, 44, 1); put16 (t, 46, 4);
  put16 (t, 48, 1); put16 (t, 52, 1); put16 (t, 54, 8);
  put16 (t, 56, 1); put16 (t, 58, 6); put16 (t, 60, (uint16_t) delta);
  put16 (t, 62, 1); put16 (t, 64, 1); put16 (t, 66, 3);
  return t;
}

/* Minimal GPOS: Thai script, mark feature, one SinglePos on @gid giving
 * (@xp, @yp) font-unit placement (40, 100 -> 4, 10 pixels at our scale). */
static std::vector<uint8_t>
make_thai_gpos (uint16_t gid, int16_t xp, int16_t yp)
{
  std::vector<uint8_t> t (72, 0);
  put16 (t, 0, 1); put16 (t, 4, 10); put16 (t, 6, 30); put16 (t, 8, 44);
  put16 (t, 10, 1);
  t[12] = 't'; t[13] = 'h'; t[14] = 'a'; t[15] = 'i';
  put16 (t, 16, 8);
  put16 (t, 18, 4); put16 (t, 20, 0);
  put16 (t, 22, 0); put16 (t, 24, 0xFFFF); put16 (t, 26, 1); put16 (t, 28, 0);
  put16 (t, 30, 1); /* featureCount */
  t[32] = 'm'; t[33] = 'a'; t[34] = 'r'; t[35] = 'k';
  put16 (t, 36, 8);
  put16 (t, 38, 0); put16 (t, 40, 1); put16 (t, 42, 0);
  put16 (t, 44, 1); put16 (t, 46, 4);
  put16 (t, 48, 1); put16 (t, 52, 1); put16 (t, 54, 8);
  put16 (t, 56, 1); put16 (t, 58, 10); put16 (t, 60, 3);
  put16 (t, 62, (uint16_t) xp); put16 (t, 64, (uint16_t) yp);
  put16 (t, 66, 1); put16 (t, 68, 1); put16 (t, 70, gid);
  return t;
}

static hb_face_t *
build_face (bool with_layout_tables)
{
  /* Glyph ids used by the cmap and assertions below. */
  enum {
    G_NOTDEF = 0, G_BASE = 1, G_DOT = 2, G_TONE = 3, G_SARA_AA = 4,
    G_BASE2 = 5, G_PUA_TONE = 6, G_LAYOUT_TONE = 7
  };

  std::vector<glyph_t> glyphs (8);
  glyphs[G_BASE].rects = {{50, 0, 450, 350}};
  glyphs[G_BASE].advance = 500;
  /* Zero-advance above-base marks are designed left of the pen, so they
   * sit back over the base once the pen reaches x = 50 pixels. */
  glyphs[G_DOT].rects = {{-320, 540, -220, 620}};
  glyphs[G_DOT].advance = 0;
  glyphs[G_TONE].rects = {{-300, 640, -180, 740}};
  glyphs[G_TONE].advance = 0;
  /* Right vowel: vertical stroke plus a serif that runs right. */
  glyphs[G_SARA_AA].rects = {{0, 0, 40, 520}, {40, 460, 560, 500}};
  glyphs[G_SARA_AA].advance = 500;
  /* Next base: body, a descender foot and a flag extending past the
   * 150-pixel logical advance of the whole word (to x = 156). */
  glyphs[G_BASE2].rects = {{50, 0, 450, 350},
			   {200, -120, 300, 0},
			   {450, 200, 560, 260}};
  glyphs[G_BASE2].advance = 500;
  /* Legacy Thai fonts carry Windows PUA glyphs for shifted marks; the
   * fallback shaper maps MAI EK (U+0E48) to U+F70A.  Give that outline a
   * location clearly distinct from the ordinary tone glyph. */
  glyphs[G_PUA_TONE].rects = {{-320, 780, -220, 860}};
  glyphs[G_PUA_TONE].advance = 0;
  /* Outline selected by the GSUB ccmp substitution in the layout font,
   * wider and higher than the ordinary tone. */
  glyphs[G_LAYOUT_TONE].rects = {{-360, 660, -160, 780}};
  glyphs[G_LAYOUT_TONE].advance = 0;

  std::map<uint32_t, uint16_t> cmap = {
    {0x0E01, G_BASE},
    {0x0E02, G_BASE2},
    {0x0E32, G_SARA_AA},
    {0x0E33, G_NOTDEF}, /* must be decomposed before cmap use */
    {0x0E48, G_TONE},
    {0x0E4D, G_DOT},
    {0xF70A, G_PUA_TONE}, /* Windows PUA MAI EK fallback */
  };

  /* ── glyf / loca ── */
  std::vector<uint8_t> glyf;
  std::vector<uint32_t> loca (glyphs.size () + 1);
  for (unsigned g = 0; g < glyphs.size (); g++)
  {
    loca[g] = glyf.size ();
    if (g == G_NOTDEF || glyphs[g].rects.empty ()) continue;

    const auto &rects = glyphs[g].rects;
    int xmin = 32767, ymin = 32767, xmax = -32768, ymax = -32768;
    unsigned npts = 0;
    for (const auto &r : rects)
    {
      xmin = std::min (xmin, r.x0); xmax = std::max (xmax, r.x1);
      ymin = std::min (ymin, r.y0); ymax = std::max (ymax, r.y1);
      npts += 4;
    }

    unsigned start = glyf.size ();
    for (unsigned k = 0; k < 5; k++) add16 (glyf, 0); /* 10-byte header */
    put16 (glyf, start + 0, rects.size ());
    put16 (glyf, start + 2, (uint16_t) xmin);
    put16 (glyf, start + 4, (uint16_t) ymin);
    put16 (glyf, start + 6, (uint16_t) xmax);
    put16 (glyf, start + 8, (uint16_t) ymax);

    unsigned endpt = 3;
    for (unsigned c = 0; c < rects.size (); c++)
    { add16 (glyf, (uint16_t) endpt); endpt += 4; }
    add16 (glyf, 0); /* instruction length */
    for (unsigned p = 0; p < npts; p++) glyf.push_back (0x01); /* on-curve, int16 deltas */

    int lx = 0;
    for (const auto &r : rects)
    {
      int xs[4] = {r.x0, r.x1, r.x1, r.x0};
      for (int i = 0; i < 4; i++)
      { add16 (glyf, (uint16_t) (int16_t) (xs[i] - lx)); lx = xs[i]; }
    }
    int ly = 0;
    for (const auto &r : rects)
    {
      int ys[4] = {r.y0, r.y0, r.y1, r.y1};
      for (int i = 0; i < 4; i++)
      { add16 (glyf, (uint16_t) (int16_t) (ys[i] - ly)); ly = ys[i]; }
    }
    if (glyf.size () & 1) glyf.push_back (0);
  }
  loca[glyphs.size ()] = glyf.size ();

  /* ── standard tables ── */
  std::vector<uint8_t> head (54, 0);
  put32 (head, 0, 0x00010000);
  put32 (head, 12, 0x5F0F3CF5);
  put16 (head, 20, 1000); /* unitsPerEm */
  put16 (head, 50, 1);    /* long loca */

  std::vector<uint8_t> hhea (36, 0);
  put32 (hhea, 0, 0x00010000);
  put16 (hhea, 4, 800);
  put16 (hhea, 6, (uint16_t) -200);
  put16 (hhea, 34, (uint16_t) glyphs.size ());

  std::vector<uint8_t> maxp (32, 0);
  put32 (maxp, 0, 0x00010000);
  put16 (maxp, 4, (uint16_t) glyphs.size ());

  std::vector<uint8_t> hmtx;
  for (const auto &g : glyphs)
  {
    int xmin = 32767;
    for (const auto &r : g.rects) xmin = std::min (xmin, r.x0);
    if (g.rects.empty ()) xmin = 0;
    add16 (hmtx, (uint16_t) g.advance);
    add16 (hmtx, (uint16_t) (int16_t) xmin); /* lsb = xMin */
  }

  std::vector<uint8_t> loca_table;
  for (uint32_t o : loca) add32 (loca_table, o);

  /* cmap format 12: one singleton group per codepoint. */
  std::vector<uint8_t> groups;
  for (const auto &kv : cmap)
  { add32 (groups, kv.first); add32 (groups, kv.first); add32 (groups, kv.second); }
  std::vector<uint8_t> sub;
  add16 (sub, 12); add16 (sub, 0);
  add32 (sub, 16 + 12 * (uint32_t) cmap.size ());
  add32 (sub, 0);
  add32 (sub, (uint32_t) cmap.size ());
  sub.insert (sub.end (), groups.begin (), groups.end ());

  std::vector<uint8_t> cmap_table;
  add16 (cmap_table, 0); add16 (cmap_table, 1);
  add16 (cmap_table, 3); add16 (cmap_table, 10); add32 (cmap_table, 12);
  cmap_table.insert (cmap_table.end (), sub.begin (), sub.end ());

  struct table_t { uint32_t tag; std::vector<uint8_t> data; };
  std::vector<table_t> tables = {
    {HB_TAG ('c', 'm', 'a', 'p'), cmap_table},
    {HB_TAG ('g', 'l', 'y', 'f'), glyf},
    {HB_TAG ('h', 'e', 'a', 'd'), head},
    {HB_TAG ('h', 'h', 'e', 'a'), hhea},
    {HB_TAG ('h', 'm', 't', 'x'), hmtx},
    {HB_TAG ('l', 'o', 'c', 'a'), loca_table},
    {HB_TAG ('m', 'a', 'x', 'p'), maxp},
  };

  std::vector<uint8_t> gsub, gpos;
  if (with_layout_tables)
  {
    gsub = make_thai_gsub (4); /* glyph 3 -> 7 */
    gpos = make_thai_gpos (7, 40, 100);
    tables.push_back ({HB_TAG ('G', 'S', 'U', 'B'), gsub});
    tables.push_back ({HB_TAG ('G', 'P', 'O', 'S'), gpos});
  }

  std::vector<uint8_t> sfnt;
  add32 (sfnt, 0x00010000);
  add16 (sfnt, (uint16_t) tables.size ());
  add16 (sfnt, 32);
  add16 (sfnt, 2);
  add16 (sfnt, (uint16_t) (tables.size () * 16));
  unsigned offset = 12 + 16 * tables.size ();
  for (const auto &t : tables)
  {
    add32 (sfnt, t.tag);
    add32 (sfnt, 0); /* checksum unused */
    add32 (sfnt, offset);
    add32 (sfnt, (uint32_t) t.data.size ());
    offset += (t.data.size () + 3) & ~3u;
  }
  for (const auto &t : tables)
  {
    sfnt.insert (sfnt.end (), t.data.begin (), t.data.end ());
    while (sfnt.size () & 3) sfnt.push_back (0);
  }

  void *bytes = malloc (sfnt.size ());
  memcpy (bytes, sfnt.data (), sfnt.size ());
  hb_blob_t *blob = hb_blob_create ((const char *) bytes, sfnt.size (),
				    HB_MEMORY_MODE_WRITABLE, bytes, free);
  hb_face_t *face = hb_face_create (blob, 0);
  hb_blob_destroy (blob);
  return face;
}

static hb_font_t *
make_test_font (bool with_layout_tables)
{
  hb_face_t *face = build_face (with_layout_tables);
  hb_font_t *font = hb_font_create (face);
  hb_face_destroy (face);
  /* 100 ppem on a 1000 upem design: design units / 10 are pixels. */
  hb_font_set_scale (font, 100, 100);
  return font;
}

/* ── Shaping and the pen placement contract (util/raster-output.hh) ─ */

struct shaped_run_t
{
  std::vector<hb_codepoint_t> glyph;
  std::vector<unsigned> cluster;
  std::vector<hb_position_t> advance_x;
  std::vector<hb_position_t> offset_x;
  std::vector<hb_position_t> offset_y;
};

static shaped_run_t
shape_thai (hb_font_t *font, const char *utf8)
{
  hb_buffer_t *buffer = hb_buffer_create ();
  hb_buffer_add_utf8 (buffer, utf8, -1, 0, -1);
  hb_buffer_set_direction (buffer, HB_DIRECTION_LTR);
  hb_buffer_set_script (buffer, HB_SCRIPT_THAI);
  hb_buffer_set_language (buffer, hb_language_from_string ("th", -1));
  hb_shape (font, buffer, nullptr, 0);

  unsigned count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buffer, &count);
  const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buffer, &count);

  shaped_run_t run;
  for (unsigned i = 0; i < count; i++)
  {
    run.glyph.push_back (info[i].codepoint);
    run.cluster.push_back (info[i].cluster);
    run.advance_x.push_back (pos[i].x_advance);
    run.offset_x.push_back (pos[i].x_offset);
    run.offset_y.push_back (pos[i].y_offset);
  }
  hb_buffer_destroy (buffer);
  return run;
}

struct place_t { hb_codepoint_t glyph; float x, y; };

/* Positions each outline at pen + positioning offset and advances the pen
 * by the font-given advance only: a mark offset never moves later glyphs.
 * This mirrors consume_glyphs() in util/raster-output.hh. */
static std::vector<place_t>
places_from_run (const shaped_run_t &run)
{
  std::vector<place_t> places;
  hb_position_t pen_x = 0;
  for (unsigned i = 0; i < run.glyph.size (); i++)
  {
    places.push_back ({run.glyph[i],
		       (float) (pen_x + run.offset_x[i]),
		       (float) run.offset_y[i]});
    pen_x += run.advance_x[i];
  }
  return places;
}

/* ── Raster helpers ────────────────────────────────────────────────── */

/* Renders @places.  With @ext == nullptr the image sizes itself from the
 * outline bounds.  @only_gid >= 0 restricts drawing to that glyph;
 * @shift_* add an extra translation to it (a caller-side offset). */
static hb_raster_image_t *
render_places (hb_font_t *font, const std::vector<place_t> &places,
	       const hb_raster_extents_t *ext,
	       float origin_x, float origin_y,
	       int only_gid = -1,
	       float shift_gid = -1.f, float shift_dx = 0.f, float shift_dy = 0.f)
{
  hb_raster_draw_t *rdr = hb_raster_draw_create_or_fail ();
  g_assert_nonnull (rdr);
  if (ext)
  {
    hb_raster_extents_t e = *ext;
    e.stride = 0;
    hb_raster_draw_set_extents (rdr, &e);
  }

  for (const auto &p : places)
  {
    if (only_gid >= 0 && (int) p.glyph != only_gid) continue;
    float dx = origin_x + p.x;
    float dy = origin_y + p.y;
    if (shift_gid >= 0.f && (int) p.glyph == (int) shift_gid)
    { dx += shift_dx; dy += shift_dy; }
    hb_raster_draw_set_transform (rdr, 1.f, 0.f, 0.f, 1.f, dx, dy);
    hb_raster_draw_glyph (rdr, font, p.glyph);
  }

  hb_raster_image_t *img = hb_raster_draw_render (rdr);
  hb_raster_draw_destroy (rdr);
  return img;
}

/* Coverage value at device pixel (@x, @y); -1 outside the image grid. */
static int
image_alpha (hb_raster_image_t *img, int x, int y)
{
  hb_raster_extents_t ext;
  hb_raster_image_get_extents (img, &ext);
  int col = x - ext.x_origin;
  int row = y - ext.y_origin;
  if (col < 0 || row < 0 ||
      (unsigned) col >= ext.width || (unsigned) row >= ext.height)
    return -1;
  return hb_raster_image_get_buffer (img)[(size_t) row * ext.stride + col];
}

/* Renders the run as one image and as @tw by @th visible-region tiles
 * composited back into the same integer pixel grid; the two ink buffers
 * must be byte identical.  Every tile uses the same fractional origin and
 * the same unrounded pen positions, so sub-pixel phase is preserved
 * across seams. */
static void
assert_tiles_match (hb_font_t *font, const std::vector<place_t> &places,
		    hb_raster_image_t *whole,
		    float origin_x, float origin_y,
		    unsigned tw, unsigned th)
{
  hb_raster_extents_t ext;
  hb_raster_image_get_extents (whole, &ext);
  const uint8_t *whole_buf = hb_raster_image_get_buffer (whole);

  std::vector<uint8_t> grid ((size_t) ext.width * ext.height, 0);

  int x_end = ext.x_origin + (int) ext.width;
  int y_end = ext.y_origin + (int) ext.height;
  for (int ty = ext.y_origin; ty < y_end; ty += (int) th)
  {
    unsigned tile_h = std::min ((int) th, y_end - ty);
    for (int tx = ext.x_origin; tx < x_end; tx += (int) tw)
    {
      unsigned tile_w = std::min ((int) tw, x_end - tx);
      hb_raster_extents_t tile_ext = {tx, ty, tile_w, tile_h, 0};
      hb_raster_image_t *tile =
	render_places (font, places, &tile_ext, origin_x, origin_y);
      g_assert_nonnull (tile);

      hb_raster_extents_t te;
      hb_raster_image_get_extents (tile, &te);
      const uint8_t *tb = hb_raster_image_get_buffer (tile);
      int col0 = te.x_origin - ext.x_origin;
      int row0 = te.y_origin - ext.y_origin;
      g_assert_cmpint (col0, >=, 0);
      g_assert_cmpint (row0, >=, 0);

      for (unsigned r = 0; r < te.height; r++)
	memcpy (grid.data () + (size_t) (row0 + r) * ext.width + col0,
		tb + (size_t) r * te.stride, te.width);
      hb_raster_image_destroy (tile);
    }
  }

  for (unsigned y = 0; y < ext.height; y++)
    for (unsigned x = 0; x < ext.width; x++)
    {
      uint8_t a = whole_buf[(size_t) y * ext.stride + x];
      uint8_t b = grid[(size_t) y * ext.width + x];
      if (a != b)
	g_error ("tile composite differs from whole at col=%u row=%u: "
		  "whole=%u tile=%u (tile %ux%u origin %d,%d)",
		  x, y, a, b, tw, th, ext.x_origin, ext.y_origin);
    }
}

/* Over a small rectangle that only @gid paints, the combined image must
 * carry exactly the coverage @gid has on the shared grid by itself. */
static void
assert_mark_coverage_preserved (hb_font_t *font,
				const std::vector<place_t> &places,
				hb_raster_image_t *whole,
				const hb_raster_extents_t &grid_ext,
				float ox, float oy, int gid,
				int x0, int y0, int x1, int y1)
{
  hb_raster_image_t *ref =
    render_places (font, places, &grid_ext, ox, oy, gid);
  g_assert_nonnull (ref);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      g_assert_cmpint (image_alpha (whole, x, y), ==,
		       image_alpha (ref, x, y));
  hb_raster_image_destroy (ref);
}

/* ── Tests ────────────────────────────────────────────────────────── */

/* Sara Am decomposition/reorder and legacy PUA selection give the exact
 * glyph sequence pinned by test/shape/.../sara-am.tests. */
static void
test_thai_shaping_sequences (void)
{
  hb_font_t *font = make_test_font (false);

  shaped_run_t run =
    shape_thai (font, "\xE0\xB8\x81\xE0\xB9\x88\xE0\xB8\xB3\xE0\xB8\x82");
  g_assert_cmpuint (run.glyph.size (), ==, 5);
  g_assert_cmpuint (run.glyph[0], ==, 1); /* base */
  g_assert_cmpuint (run.glyph[1], ==, 2); /* NIKHAHIT dot, moved front */
  g_assert_cmpuint (run.glyph[2], ==, 3); /* MAI EK tone */
  g_assert_cmpuint (run.glyph[3], ==, 4); /* SARA AA */
  g_assert_cmpuint (run.glyph[4], ==, 5); /* next base */
  /* The whole Sara Am syllable shares cluster 0; U+0E02 starts byte 9. */
  g_assert_cmpuint (run.cluster[0], ==, 0);
  g_assert_cmpuint (run.cluster[3], ==, 0);
  g_assert_cmpuint (run.cluster[4], ==, 9);
  g_assert_cmpint (run.advance_x[0], ==, 50);
  g_assert_cmpint (run.advance_x[1], ==, 0);
  g_assert_cmpint (run.advance_x[2], ==, 0);
  g_assert_cmpint (run.advance_x[3], ==, 50);
  g_assert_cmpint (run.advance_x[4], ==, 50);

  /* Without a Thai GSUB the fallback shaper substitutes the Windows PUA
   * MAI EK glyph U+F70A (gid 6) for ก ่. */
  shaped_run_t pua = shape_thai (font, "\xE0\xB8\x81\xE0\xB9\x88");
  g_assert_cmpuint (pua.glyph.size (), ==, 2);
  g_assert_cmpuint (pua.glyph[0], ==, 1);
  g_assert_cmpuint (pua.glyph[1], ==, 6);

  hb_font_destroy (font);

  /* The layout font instead selects its GSUB tone glyph (gid 7) and the
   * GPOS mark lookup gives it a real positioning offset. */
  hb_font_t *layout = make_test_font (true);
  shaped_run_t laid =
    shape_thai (layout, "\xE0\xB8\x81\xE0\xB9\x88\xE0\xB8\xB3\xE0\xB8\x82");
  g_assert_cmpuint (laid.glyph.size (), ==, 5);
  g_assert_cmpuint (laid.glyph[1], ==, 2);
  g_assert_cmpuint (laid.glyph[2], ==, 7);
  g_assert_cmpuint (laid.glyph[3], ==, 4);
  g_assert_cmpuint (laid.glyph[4], ==, 5);
  g_assert_cmpint (laid.offset_x[2], ==, 4);
  g_assert_cmpint (laid.offset_y[2], ==, 10);
  hb_font_destroy (layout);
}

/* Whole-word and chunked/composited ink agree, including at fractional
 * origins; each above-base mark keeps its own coverage, and the image
 * bounds include strokes past the logical advance. */
static void
test_thai_chunked_raster_equivalence (void)
{
  hb_font_t *font = make_test_font (false);
  shaped_run_t run =
    shape_thai (font, "\xE0\xB8\x81\xE0\xB9\x88\xE0\xB8\xB3\xE0\xB8\x82");
  std::vector<place_t> places = places_from_run (run);

  const float origins[][2] = {{0.f, 0.f}, {0.375f, 0.625f}, {0.5f, 0.25f}};
  for (const auto &o : origins)
  {
    float ox = o[0], oy = o[1];
    hb_raster_image_t *whole = render_places (font, places, nullptr, ox, oy);
    g_assert_nonnull (whole);

    hb_raster_extents_t ext;
    hb_raster_image_get_extents (whole, &ext);
    g_assert_cmpuint (ext.width, >, 100);
    g_assert_cmpuint (ext.height, >, 70);

    /* Vertical strips and 2-D tiles, including widths that cut straight
     * through the above-base marks and the right vowel. */
    assert_tiles_match (font, places, whole, ox, oy, 13, ext.height);
    assert_tiles_match (font, places, whole, ox, oy, 23, 19);
    assert_tiles_match (font, places, whole, ox, oy, 64, 31);

    /* The dot and the tone each have a fully covered core and are
     * separated by an empty row: they keep distinct coverage. */
    g_assert_cmpint (image_alpha (whole, 23, 58), ==, 255); /* dot core */
    g_assert_cmpint (image_alpha (whole, 26, 69), ==, 255); /* tone core */
    g_assert_cmpint (image_alpha (whole, 24, 63), ==, 0);    /* gap */

    /* Cores match the mark rendered alone onto the identical grid. */
    assert_mark_coverage_preserved (font, places, whole, ext, ox, oy, 2,
				    21, 56, 26, 60);
    assert_mark_coverage_preserved (font, places, whole, ext, ox, oy, 3,
				    23, 67, 29, 72);

    /* A non-integer origin turns an integer-aligned outline edge into a
     * partial-coverage column; snapping the pen per chunk would make it
     * fully covered or empty. */
    if (ox == 0.5f)
    {
      int edge = image_alpha (whole, 5, 17); /* base left edge at x = 5.5 */
      g_assert_cmpint (edge, >, 16);
      g_assert_cmpint (edge, <, 240);
      g_assert_cmpint (image_alpha (whole, 12, 17), ==, 255);
    }

    /* Logical advance is 150 px; BASE2's flag reaches x = 156. */
    g_assert_cmpint (places.back ().x + run.advance_x.back (), ==, 150);
    g_assert_cmpint (image_alpha (whole, 152, 23), ==, 255);
    g_assert_cmpint (ext.x_origin + (int) ext.width, >=, 156);

    hb_raster_image_destroy (whole);
  }

  hb_font_destroy (font);
}

/* Legacy PUA outline lands where its own geometry says, and a mark
 * positioning offset moves only that mark's ink. */
static void
test_thai_legacy_pua_outline_and_pen (void)
{
  hb_font_t *font = make_test_font (false);
  shaped_run_t run = shape_thai (font, "\xE0\xB8\x81\xE0\xB9\x88\xE0\xB8\x82");
  g_assert_cmpuint (run.glyph.size (), ==, 3);
  g_assert_cmpuint (run.glyph[1], ==, 6);
  std::vector<place_t> places = places_from_run (run);

  hb_raster_image_t *img = render_places (font, places, nullptr, 0.f, 0.f);
  g_assert_nonnull (img);

  /* PUA tone outline core, and nothing in the ordinary tone slot. */
  g_assert_cmpint (image_alpha (img, 23, 82), ==, 255);
  g_assert_cmpint (image_alpha (img, 26, 69), ==, 0);

  /* Its core carries exactly the standalone PUA outline coverage. */
  hb_raster_extents_t pua_ext;
  hb_raster_image_get_extents (img, &pua_ext);
  assert_mark_coverage_preserved (font, places, img, pua_ext, 0.f, 0.f,
				  6, 20, 80, 26, 84);

  /* Following base starts at pen 50 (base advance only). */
  g_assert_cmpint (image_alpha (img, 54, 17), ==, 0);
  g_assert_cmpint (image_alpha (img, 56, 17), ==, 255);
  g_assert_cmpint (image_alpha (img, 75, -6), ==, 255); /* foot */

  /* A caller-applied positioning offset on the mark moves its ink by the
   * same integer amount and leaves the next base's pixels untouched. */
  hb_raster_image_t *shifted =
    render_places (font, places, nullptr, 0.f, 0.f,
		   -1, 6.f, 7.f, 9.f);
  g_assert_nonnull (shifted);
  g_assert_cmpint (image_alpha (shifted, 23, 82), ==, 0);
  g_assert_cmpint (image_alpha (shifted, 30, 91), ==, 255);
  g_assert_cmpint (image_alpha (shifted, 56, 17), ==, 255);
  g_assert_cmpint (image_alpha (shifted, 75, -6), ==, 255);

  hb_raster_image_destroy (shifted);
  hb_raster_image_destroy (img);
  hb_font_destroy (font);
}

/* For the GSUB/GPOS font the selected layout outline is shifted by the
 * font-given mark offset, while later glyphs still start at the pen
 * derived from advances alone. */
static void
test_thai_layout_offset_does_not_move_pen (void)
{
  hb_font_t *font = make_test_font (true);
  shaped_run_t run =
    shape_thai (font, "\xE0\xB8\x81\xE0\xB9\x88\xE0\xB8\xB3\xE0\xB8\x82");
  g_assert_cmpuint (run.glyph[2], ==, 7);
  g_assert_cmpint (run.offset_x[2], ==, 4);
  g_assert_cmpint (run.offset_y[2], ==, 10);
  std::vector<place_t> places = places_from_run (run);

  hb_raster_image_t *img = render_places (font, places, nullptr, 0.f, 0.f);
  g_assert_nonnull (img);

  /* Layout tone rect is y 66..78; with the (4, 10) offset its core is at
   * y ~76..88.  The shifted core is covered and the unshifted slot is
   * empty; the ordinary tone slot stays empty too. */
  g_assert_cmpint (image_alpha (img, 28, 82), ==, 255);
  g_assert_cmpint (image_alpha (img, 28, 70), ==, 0);
  g_assert_cmpint (image_alpha (img, 26, 69), ==, 0);

  /* The shifted core keeps exactly the selected GSUB outline coverage. */
  hb_raster_extents_t layout_ext;
  hb_raster_image_get_extents (img, &layout_ext);
  assert_mark_coverage_preserved (font, places, img, layout_ext, 0.f, 0.f,
				  7, 21, 79, 35, 85);

  /* Next base pen = 100 (50 base + 50 SARA AA); the mark's 4/10 offset is
   * not accumulated into it.  Probe the left edge, the foot and the row
   * ten pixels above the body where an accumulated y offset would move
   * the base. */
  g_assert_cmpint (image_alpha (img, 104, 17), ==, 0);
  g_assert_cmpint (image_alpha (img, 106, 17), ==, 255);
  g_assert_cmpint (image_alpha (img, 125, -6), ==, 255);
  g_assert_cmpint (image_alpha (img, 125, 40), ==, 0);

  hb_raster_image_destroy (img);
  hb_font_destroy (font);
}

/* The same whole-vs-chunk contract against the CFF font used by
 * test/shape/data/in-house/tests/sara-am.tests.  Best effort: when the
 * source tree font cannot be located (standalone compiler/probe runs),
 * skip rather than abort. */
static hb_face_t *
open_sara_am_font_if_present (void)
{
  const char *rel = "shape/data/in-house/fonts/"
		    "63a539a90a371ccf028dc2dcced9b63b07163be7.ttf";
  std::vector<std::string> candidates;
  if (const char *sd = g_getenv ("G_TEST_SRCDIR"))
    candidates.push_back (std::string (sd) + "/../" + rel);
  candidates.push_back (std::string ("test/") + rel);
  candidates.push_back (std::string ("../test/") + rel);
  candidates.push_back (std::string ("../") + rel);
  candidates.push_back (std::string ("../../test/") + rel);

  for (const auto &path : candidates)
  {
    hb_blob_t *blob = hb_blob_create_from_file_or_fail (path.c_str ());
    if (!blob) continue;
    hb_face_t *face = hb_face_create (blob, 0);
    hb_blob_destroy (blob);
    if (face && hb_face_get_glyph_count (face) > 0) return face;
    hb_face_destroy (face);
  }
  return nullptr;
}

static void
test_thai_repo_sara_am_font (void)
{
  hb_face_t *face = open_sara_am_font_if_present ();
  if (!face)
  {
    g_test_skip ("in-tree sara-am test font not reachable from this cwd");
    return;
  }
  hb_font_t *font = hb_font_create (face);
  hb_face_destroy (face);
  hb_font_set_scale (font, 1000, 1000); /* design units map 1:1 */

  shaped_run_t run =
    shape_thai (font, "\xE0\xB8\x81\xE0\xB9\x88\xE0\xB8\xB3");
  g_assert_cmpuint (run.glyph.size (), ==, 4);
  g_assert_cmpint (run.advance_x[0], ==, 500);
  g_assert_cmpint (run.advance_x[1], ==, 0);
  g_assert_cmpint (run.advance_x[2], ==, 0);
  g_assert_cmpint (run.advance_x[3], ==, 500);

  std::vector<place_t> places = places_from_run (run);
  hb_raster_image_t *whole =
    render_places (font, places, nullptr, 0.3125f, 0.1875f);
  g_assert_nonnull (whole);
  hb_raster_extents_t ext;
  hb_raster_image_get_extents (whole, &ext);
  g_assert_cmpuint (ext.width, >, 0);
  g_assert_cmpuint (ext.height, >, 0);
  assert_tiles_match (font, places, whole, 0.3125f, 0.1875f, 47, ext.height);

  hb_raster_image_destroy (whole);
  hb_font_destroy (font);
}

int
main (int argc, char **argv)
{
  hb_test_init (&argc, &argv);

  hb_test_add (test_thai_shaping_sequences);
  hb_test_add (test_thai_chunked_raster_equivalence);
  hb_test_add (test_thai_legacy_pua_outline_and_pen);
  hb_test_add (test_thai_layout_offset_does_not_move_pen);
  hb_test_add (test_thai_repo_sara_am_font);

  return hb_test_run ();
}
