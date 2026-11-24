/*
 * Copyright © 2026  Google, Inc.
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
 *
 * Google Author(s): Behdad Esfahbod
 */

#include "hb-test.h"

/* Integration tests for partial (item) shaping of Arabic text.
 *
 * An editor that redraws only the just-edited word segment passes the
 * whole paragraph to hb_buffer_add_utf8() but restricts the shaped
 * item to the edited range; HarfBuzz then uses the surrounding text
 * only as context.  These tests check, for right-to-left Arabic text:
 *
 *  - letters of the redrawn segment keep the contextual (joining)
 *    forms they have inside the whole word;
 *  - transparent marks still belong to the original text: they are
 *    skipped by the joining scan and stay in their base letter's
 *    cluster;
 *  - the author's ZWJ/ZWNJ join-control intent, inside the segment or
 *    in its context, is reflected in the glyphs;
 *  - glyph clusters are byte offsets into the original paragraph, so
 *    the application can recover the segment's range in the word.
 *
 * The tests shape with a synthetic font whose GSUB implements the four
 * Arabic joining features as single substitutions of the BEH glyph
 * into a distinct glyph per form, so the joining form of every letter
 * can be read straight off its glyph ID. */

#define BEH   "\xD8\xA8"	/* U+0628 ARABIC LETTER BEH; dual-joining */
#define FATHA "\xD9\x8E"	/* U+064E ARABIC FATHA; transparent mark */
#define ZWNJ  "\xE2\x80\x8C"	/* U+200C ZERO WIDTH NON-JOINER */
#define ZWJ   "\xE2\x80\x8D"	/* U+200D ZERO WIDTH JOINER */

enum {
  GID_BEH      = 1,
  GID_FATHA    = 2,
  GID_BEH_ISOL = 3,
  GID_BEH_FINA = 4,
  GID_BEH_MEDI = 5,
  GID_BEH_INIT = 6
};

static void
put16 (char *p, unsigned v)
{
  p[0] = (char) (v >> 8);
  p[1] = (char) v;
}

static hb_blob_t *
create_arabic_forms_gsub_blob (void)
{
  /* Header(10) ScriptList(26) FeatureList(50) LookupList(10 + 4*22). */
  char *gsub = calloc (184, 1);
  char *p = gsub;

  /* Header */
  put16 (p, 1);		/* version 1.0 */
  put16 (p + 4, 10);	/* ScriptList offset */
  put16 (p + 6, 36);	/* FeatureList offset */
  put16 (p + 8, 86);	/* LookupList offset */
  p += 10;

  /* ScriptList: 'arab' with a default LangSys using all four features */
  put16 (p, 1);			/* scriptCount */
  memcpy (p + 2, "arab", 4);
  put16 (p + 6, 8);		/* Script offset */
  put16 (p + 8, 4);		/* defaultLangSysOffset */
  put16 (p + 10, 0);		/* langSysCount */
  /* LangSys */
  put16 (p + 12, 0);		/* lookupOrder */
  put16 (p + 14, 0xFFFF);	/* reqFeatureIndex */
  put16 (p + 16, 4);		/* featureIndexCount */
  put16 (p + 18, 0);
  put16 (p + 20, 1);
  put16 (p + 22, 2);
  put16 (p + 24, 3);
  p += 26;

  /* FeatureList: isol/fina/medi/init, feature i uses lookup i */
  static const char tags[4][4] = {"isol", "fina", "medi", "init"};
  put16 (p, 4);			/* featureCount */
  for (unsigned i = 0; i < 4; i++)
  {
    memcpy (p + 2 + 6 * i, tags[i], 4);
    put16 (p + 6 + 6 * i, 26 + 6 * i);
  }
  p += 2 + 4 * 6;
  for (unsigned i = 0; i < 4; i++)
  {
    put16 (p, 0);		/* featureParams */
    put16 (p + 2, 1);		/* lookupIndexCount */
    put16 (p + 4, i);		/* lookupListIndex */
    p += 6;
  }

  /* LookupList: four SingleSubst lookups, BEH -> BEH.<form> */
  put16 (p, 4);			/* lookupCount */
  for (unsigned i = 0; i < 4; i++)
    put16 (p + 2 + 2 * i, 10 + 22 * i);
  p += 10;
  for (unsigned i = 0; i < 4; i++)
  {
    put16 (p, 1);		/* lookupType: SingleSubst */
    put16 (p + 2, 0);		/* lookupFlag */
    put16 (p + 4, 1);		/* subTableCount */
    put16 (p + 6, 8);		/* subtable offset */
    put16 (p + 8, 1);		/* SingleSubstFormat1 */
    put16 (p + 10, 6);		/* coverage offset */
    put16 (p + 12, 2 + i);	/* deltaGlyphID: 1 -> 3, 4, 5, 6 */
    put16 (p + 14, 1);		/* CoverageFormat1 */
    put16 (p + 16, 1);		/* glyphCount */
    put16 (p + 18, GID_BEH);
    p += 22;
  }

  return hb_blob_create_or_fail (gsub, 184, HB_MEMORY_MODE_WRITABLE, gsub, free);
}

static hb_face_t *
create_arabic_forms_face (void)
{
  /* cmap format 4: U+0628 -> glyph 1, U+064E -> glyph 2. */
  static const char cmap[] = {
    0, 0, 0, 1,  0, 3, 0, 1,  0, 0, 0, 12,	/* one subtable, platform 3, encoding 1 */
    0, 4, 0, 40, 0, 0,				/* format 4, length, language */
    0, 6, 0, 4,  0, 1, 0, 2,			/* segCountX2, searchRange, entrySelector, rangeShift */
    0x06, 0x28,  0x06, 0x4E,  0xFF, 0xFF,		/* endCode */
    0, 0,					/* reservedPad */
    0x06, 0x28,  0x06, 0x4E,  0xFF, 0xFF,		/* startCode */
    0xF9, 0xD9,  0xF9, 0xB4,  0, 1,		/* idDelta */
    0, 0,  0, 0,  0, 0				/* idRangeOffset */
  };

  hb_face_t *face = hb_face_builder_create ();
  HB_FACE_ADD_TABLE (face, "cmap", cmap);
  hb_blob_t *gsub = create_arabic_forms_gsub_blob ();
  hb_face_builder_add_table (face, HB_TAG ('G','S','U','B'), gsub);
  hb_blob_destroy (gsub);
  hb_face_set_glyph_count (face, 7);
  return face;
}

typedef struct {
  hb_codepoint_t gid;
  unsigned int cluster;
} shaped_glyph_t;

/* Shapes text[item_offset .. item_offset + item_length) of text, the
 * way an editor reshapes only the edited word segment while handing
 * the whole paragraph to HarfBuzz for context. */
static unsigned int
shape_item (hb_font_t   *font,
	    const char  *text,
	    unsigned int text_length,
	    unsigned int item_offset,
	    unsigned int item_length,
	    shaped_glyph_t **glyphs)
{
  hb_buffer_t *buffer = hb_buffer_create ();
  hb_buffer_add_utf8 (buffer, text, text_length, item_offset, item_length);
  hb_buffer_guess_segment_properties (buffer);

  /* The text is Arabic; the run must come out right-to-left. */
  g_assert_cmpint (hb_buffer_get_direction (buffer), ==, HB_DIRECTION_RTL);

  const char *shaper_list[] = {"ot", NULL};
  g_assert_true (hb_shape_full (font, buffer, NULL, 0, shaper_list));

  unsigned int len;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buffer, &len);
  shaped_glyph_t *out = g_new (shaped_glyph_t, len);
  for (unsigned int i = 0; i < len; i++)
  {
    out[i].gid = info[i].codepoint;
    out[i].cluster = info[i].cluster;
  }
  hb_buffer_destroy (buffer);

  *glyphs = out;
  return len;
}

static hb_codepoint_t
gid_at_cluster (const shaped_glyph_t *glyphs, unsigned int len, unsigned int cluster)
{
  for (unsigned int i = 0; i < len; i++)
    if (glyphs[i].cluster == cluster)
      return glyphs[i].gid;
  g_assert_not_reached ();
  return 0;
}

static hb_bool_t
has_glyph (const shaped_glyph_t *glyphs, unsigned int len,
	   unsigned int cluster, hb_codepoint_t gid)
{
  for (unsigned int i = 0; i < len; i++)
    if (glyphs[i].cluster == cluster && glyphs[i].gid == gid)
      return TRUE;
  return FALSE;
}

/* The editor reshapes only the middle of a word; the reshaped letters
 * must keep the contextual forms they have in the whole word, because
 * the neighbouring letters still join with them. */
static void
test_partial_item_keeps_joining_forms (void)
{
  /* BEH x4; letters at byte offsets 0, 2, 4, 6. */
  const char word[] = BEH BEH BEH BEH;
  const unsigned int word_len = 8;

  hb_face_t *face = create_arabic_forms_face ();
  hb_font_t *font = hb_font_create (face);
  hb_face_destroy (face);

  /* The whole word shapes as initial, two medials, final. */
  shaped_glyph_t *full;
  unsigned int full_len = shape_item (font, word, word_len, 0, word_len, &full);
  g_assert_cmpuint (full_len, ==, 4);
  g_assert_cmpuint (gid_at_cluster (full, full_len, 0), ==, GID_BEH_INIT);
  g_assert_cmpuint (gid_at_cluster (full, full_len, 2), ==, GID_BEH_MEDI);
  g_assert_cmpuint (gid_at_cluster (full, full_len, 4), ==, GID_BEH_MEDI);
  g_assert_cmpuint (gid_at_cluster (full, full_len, 6), ==, GID_BEH_FINA);

  /* Redrawing only the middle two letters yields exactly the glyphs
   * the whole word has at those clusters. */
  shaped_glyph_t *part;
  unsigned int part_len = shape_item (font, word, word_len, 2, 4, &part);
  g_assert_cmpuint (part_len, ==, 2);
  for (unsigned int i = 0; i < part_len; i++)
    g_assert_cmpuint (part[i].gid, ==, gid_at_cluster (full, full_len, part[i].cluster));

  /* Shaped without the surrounding context, the same two letters would
   * take the wrong (initial/final) forms. */
  shaped_glyph_t *bare;
  unsigned int bare_len = shape_item (font, word + 2, 4, 0, 4, &bare);
  g_assert_cmpuint (bare_len, ==, 2);
  g_assert_cmpuint (gid_at_cluster (bare, bare_len, 0), ==, GID_BEH_INIT);
  g_assert_cmpuint (gid_at_cluster (bare, bare_len, 2), ==, GID_BEH_FINA);

  g_free (full);
  g_free (part);
  g_free (bare);
  hb_font_destroy (font);
}

/* Transparent marks neither break the joining of the letters around
 * them nor leave their base letter's cluster. */
static void
test_partial_item_transparent_marks (void)
{
  hb_face_t *face = create_arabic_forms_face ();
  hb_font_t *font = hb_font_create (face);
  hb_face_destroy (face);

  /* BEH FATHA BEH; byte offsets 0, 2, 4.  The mark is transparent to
   * joining, so the two letters still join across it. */
  const char word[] = BEH FATHA BEH;
  const unsigned int word_len = 6;

  shaped_glyph_t *full;
  unsigned int full_len = shape_item (font, word, word_len, 0, word_len, &full);
  /* The letters join across the mark, and the mark shares its base
   * letter's cluster. */
  g_assert_true (has_glyph (full, full_len, 0, GID_BEH_INIT));
  g_assert_true (has_glyph (full, full_len, 0, GID_FATHA));
  g_assert_true (has_glyph (full, full_len, 4, GID_BEH_FINA));

  /* Redrawing only the last letter: the mark in the pre-context is
   * skipped by the joining scan, so the letter still takes its final
   * form from the letter before the mark. */
  shaped_glyph_t *part;
  unsigned int part_len = shape_item (font, word, word_len, 4, 2, &part);
  g_assert_cmpuint (part_len, ==, 1);
  g_assert_cmpuint (part[0].gid, ==, GID_BEH_FINA);
  g_assert_cmpuint (part[0].cluster, ==, 4);
  g_free (part);

  /* BEH BEH FATHA; byte offsets 0, 2, 4.  Redrawing the second letter
   * together with its mark: the mark stays in the cluster of its base
   * letter, at the letter's offset in the original word. */
  const char word2[] = BEH BEH FATHA;
  unsigned int len = shape_item (font, word2, 6, 2, 4, &part);
  g_assert_cmpuint (len, ==, 2);
  g_assert_true (has_glyph (part, len, 2, GID_BEH_FINA));
  g_assert_true (has_glyph (part, len, 2, GID_FATHA));
  for (unsigned int i = 0; i < len; i++)
    g_assert_cmpuint (part[i].cluster, ==, 2);
  g_free (part);

  g_free (full);
  hb_font_destroy (font);
}

/* The author's ZWJ/ZWNJ join-control intent is honoured, whether the
 * control character sits inside the redrawn segment or in its context. */
static void
test_partial_item_join_controls (void)
{
  hb_face_t *face = create_arabic_forms_face ();
  hb_font_t *font = hb_font_create (face);
  hb_face_destroy (face);

  shaped_glyph_t *glyphs;
  unsigned int len;

  /* BEH ZWNJ BEH; byte offsets 0, 2, 5.  The non-joiner breaks the
   * connection: both letters take the isolated form. */
  const char broken[] = BEH ZWNJ BEH;
  len = shape_item (font, broken, 7, 0, 7, &glyphs);
  g_assert_cmpuint (len, ==, 2);
  for (unsigned int i = 0; i < len; i++)
    g_assert_cmpuint (glyphs[i].gid, ==, GID_BEH_ISOL);
  g_free (glyphs);

  /* BEH ZWJ BEH: the joiner forces the connection. */
  const char joined[] = BEH ZWJ BEH;
  len = shape_item (font, joined, 7, 0, 7, &glyphs);
  g_assert_true (has_glyph (glyphs, len, 0, GID_BEH_INIT));
  g_assert_true (has_glyph (glyphs, len, 5, GID_BEH_FINA));
  g_free (glyphs);

  /* The same controls in the pre-context steer the redrawn letter:
   * after ZWNJ it stays isolated, after ZWJ it joins. */
  len = shape_item (font, broken, 7, 5, 2, &glyphs);
  g_assert_cmpuint (len, ==, 1);
  g_assert_cmpuint (glyphs[0].gid, ==, GID_BEH_ISOL);
  g_assert_cmpuint (glyphs[0].cluster, ==, 5);
  g_free (glyphs);

  len = shape_item (font, joined, 7, 5, 2, &glyphs);
  g_assert_cmpuint (len, ==, 1);
  g_assert_cmpuint (glyphs[0].gid, ==, GID_BEH_FINA);
  g_assert_cmpuint (glyphs[0].cluster, ==, 5);
  g_free (glyphs);

  /* A ZWJ inside the redrawn segment forces the connection too:
   * trailing ZWJ gives the initial form, leading ZWJ the final.  The
   * invisible control shares its cluster with the letter it steers. */
  const char trailing[] = BEH ZWJ;
  len = shape_item (font, trailing, 5, 0, 5, &glyphs);
  g_assert_true (has_glyph (glyphs, len, 0, GID_BEH_INIT));
  g_free (glyphs);

  const char leading[] = ZWJ BEH;
  len = shape_item (font, leading, 5, 0, 5, &glyphs);
  g_assert_true (has_glyph (glyphs, len, 0, GID_BEH_FINA));
  g_free (glyphs);

  hb_font_destroy (font);
}

/* Glyph clusters are byte offsets into the original paragraph, so the
 * application recovers where the redrawn segment sits in the word. */
static void
test_partial_item_cluster_range (void)
{
  /* BEH x4; letters at byte offsets 0, 2, 4, 6. */
  const char word[] = BEH BEH BEH BEH;
  const unsigned int item_offset = 2, item_length = 4;

  hb_face_t *face = create_arabic_forms_face ();
  hb_font_t *font = hb_font_create (face);
  hb_face_destroy (face);

  shaped_glyph_t *glyphs;
  unsigned int len = shape_item (font, word, 8, item_offset, item_length, &glyphs);
  g_assert_cmpuint (len, ==, 2);

  /* Clusters are not rebased to the item: they still index the
   * original word, and their span is exactly the redrawn range. */
  unsigned int first = UINT_MAX, last = 0;
  for (unsigned int i = 0; i < len; i++)
  {
    g_assert_cmpuint (glyphs[i].cluster, >=, item_offset);
    g_assert_cmpuint (glyphs[i].cluster, <, item_offset + item_length);
    first = MIN (first, glyphs[i].cluster);
    last = MAX (last, glyphs[i].cluster);
  }
  g_assert_cmpuint (first, ==, item_offset);
  /* The last letter starts two bytes (its UTF-8 length) before the
   * end of the redrawn range. */
  g_assert_cmpuint (last, ==, item_offset + item_length - 2);

  g_free (glyphs);
  hb_font_destroy (font);
}

int
main (int argc, char **argv)
{
  hb_test_init (&argc, &argv);

  hb_test_add (test_partial_item_keeps_joining_forms);
  hb_test_add (test_partial_item_transparent_marks);
  hb_test_add (test_partial_item_join_controls);
  hb_test_add (test_partial_item_cluster_range);

  return hb_test_run ();
}
