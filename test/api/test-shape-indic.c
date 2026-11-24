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

#include "hb-test.h"

/*
 * Devanagari shaping contracts needed by an editor that shapes a syllable
 * while it is still being typed and that reuses shaping results after a
 * line break:
 *
 *  - pre-base vowels and Reph attach to a specific base consonant and end
 *    up in a specific display order;
 *  - ZWJ/ZWNJ keep the writing intent (forced conjunct / visible halant);
 *  - clusters and the unsafe-to-break flags identify the original text
 *    boundaries at which a run may be split into independently shaped
 *    lines whose concatenation still reproduces the full shaping;
 *  - a broken syllable without a base gets its own dotted-circle base that
 *    belongs to that fragment, while a following syllable shapes alone.
 *
 * Expectations below are tied to specific fonts shipped in the tree so
 * that glyph names distinguish a real Reph/conjunct substitution from
 * leftover visible Ra/Halant.  Codepoint clusters are used throughout
 * (hb_buffer_add_codepoints counts codepoints, not UTF-8 bytes).
 */

/* Full Noto Sans Devanagari. */
#define NOTO_DEVA_FONT \
	"../subset/data/fonts/NotoSansDevanagari-Regular.ttf"

/* Subset used by test/shape/data/in-house/tests/indic-broken-cluster-reph.tests,
 * it actually contains the reph-deva substitution and a dotted circle. */
#define REPH_FONT \
	"../shape/data/in-house/fonts/40974b46edb7b503175a747a2c385ccd443c63f1.ttf"

/* Subset used by test/shape/data/in-house/tests/indic-joiners.tests. */
#define JOINER_FONT \
	"../shape/data/in-house/fonts/8116e5d8fedfbec74e45dc350d2416d810bed8c4.ttf"

enum devanagari_codepoints {
  CP_RA    = 0x0930, /* RA */
  CP_H     = 0x094D, /* DEVANAGARI SIGN VIRAMA / halant */
  CP_KA    = 0x0915,
  CP_TTA   = 0x091F,
  CP_YA    = 0x092F,
  CP_I     = 0x093F, /* short I vowel sign, reordered before its base */
  CP_ZWNJ  = 0x200C,
  CP_ZWJ   = 0x200D,
  CP_SPACE = 0x0020,
};

static hb_font_t *
test_open_font (const char *path, hb_face_t **face)
{
  *face = hb_test_open_font_file (path);
  return hb_font_create (*face);
}

static void
test_close_font (hb_font_t *font, hb_face_t *face)
{
  hb_font_destroy (font);
  hb_face_destroy (face);
}

typedef struct
{
  const char  *name;    /* expected glyph name, or NULL to ignore */
  unsigned int cluster; /* expected codepoint cluster */
  int          unsafe;  /* 1: unsafe-to-break, 0: safe-to-break, -1: don't check */
} glyph_expect_t;

static void
shape_expect (hb_font_t                    *font,
	      const hb_codepoint_t          *cps,
	      unsigned int                   n,
	      hb_buffer_cluster_level_t      level,
	      const char                    *feature_string,
	      const glyph_expect_t          *expect,
	      unsigned int                   n_expect)
{
  hb_buffer_t *buffer = hb_buffer_create ();
  hb_buffer_set_cluster_level (buffer, level);
  hb_buffer_add_codepoints (buffer, cps, n, 0, n);
  hb_buffer_set_direction (buffer, HB_DIRECTION_LTR);
  hb_buffer_set_script (buffer, HB_SCRIPT_DEVANAGARI);
  hb_buffer_set_language (buffer, hb_language_from_string ("hi", -1));

  hb_feature_t features[8];
  unsigned int n_features = 0;
  char feature_copy[128];
  if (feature_string)
  {
    g_strlcpy (feature_copy, feature_string, sizeof feature_copy);
    char *tok = strtok (feature_copy, ",");
    while (tok && n_features < G_N_ELEMENTS (features))
    {
      if (hb_feature_from_string (tok, -1, &features[n_features]))
	n_features++;
      tok = strtok (NULL, ",");
    }
  }

  hb_shape (font, buffer, n_features ? features : NULL, n_features);

  unsigned int len;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buffer, &len);
  g_assert_cmpuint (len, ==, n_expect);

  for (unsigned int i = 0; i < len; i++)
  {
    char name[64] = {0};
    hb_font_get_glyph_name (font, info[i].codepoint, name, sizeof name);

    if (expect[i].name && strcmp (name, expect[i].name) != 0)
    {
      g_test_message ("glyph %u: expected '%s', got '%s'",
		     i, expect[i].name, name);
      g_assert_cmpstr (name, ==, expect[i].name);
    }

    g_assert_cmpuint (info[i].cluster, ==, expect[i].cluster);

    hb_glyph_flags_t flags = hb_glyph_info_get_glyph_flags (&info[i]);
    gboolean unsafe = (flags & HB_GLYPH_FLAG_UNSAFE_TO_BREAK) != 0;
    if (expect[i].unsafe == 1)
      g_assert_true (unsafe);
    else if (expect[i].unsafe == 0)
      g_assert_false (unsafe);
  }

  hb_buffer_destroy (buffer);
}

/* Shape text[0..text_len) and return the buffer.  item_offset/item_len
 * select a fragment while keeping clusters relative to text, exactly as
 * an editor shaping a line would. */
static hb_buffer_t *
shape_fragment (hb_font_t           *font,
		const hb_codepoint_t *text,
		unsigned int         text_len,
		unsigned int         item_offset,
		unsigned int         item_len)
{
  hb_buffer_t *buffer = hb_buffer_create ();
  hb_buffer_set_cluster_level (buffer,
			      HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
  hb_buffer_add_codepoints (buffer, text, text_len, item_offset, item_len);
  hb_buffer_set_direction (buffer, HB_DIRECTION_LTR);
  hb_buffer_set_script (buffer, HB_SCRIPT_DEVANAGARI);
  hb_buffer_set_language (buffer, hb_language_from_string ("hi", -1));
  hb_shape (font, buffer, NULL, 0);
  return buffer;
}

/* Mirror the forward scan in hb_buffer_verify_unsafe_to_break(): a cut is
 * taken at the start of a new cluster unless that first glyph of the new
 * cluster is marked unsafe-to-break.  Clusters are codepoint indices. */
static unsigned int
collect_safe_cuts (hb_buffer_t *buffer, unsigned int *cuts, unsigned int max_cuts)
{
  unsigned int num_glyphs;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buffer, &num_glyphs);
  unsigned int num_cuts = 0;

  for (unsigned int end = 1; end < num_glyphs + 1; end++)
  {
    if (end < num_glyphs &&
	(info[end].cluster == info[end - 1].cluster ||
	 (hb_glyph_info_get_glyph_flags (&info[end]) &
	  HB_GLYPH_FLAG_UNSAFE_TO_BREAK)))
      continue;

    /* The final segment ends at the end of the text; only record
     * internal cuts here. */
    if (end < num_glyphs && num_cuts < max_cuts)
      cuts[num_cuts++] = info[end].cluster;
  }

  return num_cuts;
}

static unsigned int
count_named_glyphs (hb_font_t *font, hb_buffer_t *buffer, const char *wanted)
{
  unsigned int len;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buffer, &len);
  unsigned int count = 0;
  for (unsigned int i = 0; i < len; i++)
  {
    char name[64] = {0};
    hb_font_get_glyph_name (font, info[i].codepoint, name, sizeof name);
    if (strcmp (name, wanted) == 0)
      count++;
  }
  return count;
}

/* A pre-base vowel is typed after its consonant but displayed before it.
 * While the syllable is being typed the vowel must attach to that
 * preceding base, not leap onto a following consonant. */
static void
test_indic_prebase_vowel_editing (void)
{
  hb_face_t *face;
  hb_font_t *font = test_open_font (NOTO_DEVA_FONT, &face);

  /* Only KA typed so far. */
  {
    const hb_codepoint_t s[] = {CP_KA};
    const glyph_expect_t e[] = {
      {"kadeva", 0, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  /* KA + I: the I-matra is the first displayed glyph, yet its cluster is
   * its own source index (1) and the boundary before it is unsafe; the
   * base KA keeps cluster 0. */
  {
    const hb_codepoint_t s[] = {CP_KA, CP_I};
    const glyph_expect_t e[] = {
      {"ivowelsign03deva", 1, 1},
      {"kadeva",           0, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  /* KA + I + KA: the matra still belongs to the first KA (cluster 0);
   * the newly typed KA starts a separate syllable. */
  {
    const hb_codepoint_t s[] = {CP_KA, CP_I, CP_KA};
    const glyph_expect_t e[] = {
      {"ivowelsign03deva", 1, 1},
      {"kadeva",           0, 0},
      {"kadeva",           2, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  test_close_font (font, face);
}

/* Reph only moves after the base once Ra+Halant actually substitutes to a
 * Reph glyph.  A leading Ra candidate, an incomplete Ra+Halant prefix, or a
 * font/feature path without the substitution must keep visible Ra/Halant
 * in logical order instead. */
static void
test_indic_reph_editing (void)
{
  hb_face_t *face;
  hb_font_t *font = test_open_font (NOTO_DEVA_FONT, &face);

  /* Just RA. */
  {
    const hb_codepoint_t s[] = {CP_RA};
    const glyph_expect_t e[] = {
      {"radeva", 0, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  /* RA + Halant, still no base consonant: visible Ra + visible virama,
   * no Reph formed.  Clusters differ (0 vs 1) but the boundary is
   * explicitly unsafe-to-break, so cluster inequality is not safety. */
  {
    const hb_codepoint_t s[] = {CP_RA, CP_H};
    const glyph_expect_t e[] = {
      {"radeva",    0, 0},
      {"viramadeva", 1, 1},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  /* RA + Halant + KA: real Reph substitution; in display/buffer order the
   * base KA comes first and rephdeva follows it (positioned above it),
   * while the Reph cluster still points at the original RA index 0. */
  {
    const hb_codepoint_t s[] = {CP_RA, CP_H, CP_KA};
    const glyph_expect_t e[] = {
      {"kadeva",   2, 1},
      {"rephdeva", 0, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  /* Same string with rphf disabled: no ligated Reph, so RA, Halant and KA
   * stay visible in logical order and are not reordered. */
  {
    const hb_codepoint_t s[] = {CP_RA, CP_H, CP_KA};
    const glyph_expect_t e[] = {
      {"radeva",     0, 0},
      {"viramadeva", 1, 1},
      {"kadeva",     2, 1},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, "-rphf",
		  e, G_N_ELEMENTS (e));
  }

  test_close_font (font, face);

  /* The dedicated in-house font really contains reph-deva and a dotted
   * circle.  RA + Halant + I is a broken syllable (no base consonant):
   * the shaper inserts the dotted circle as the base and forms the Reph
   * on it, giving I-matra, dotted circle, Reph. */
  font = test_open_font (REPH_FONT, &face);
  {
    const hb_codepoint_t s[] = {CP_RA, CP_H};
    const glyph_expect_t e[] = {
      {"ra-deva",     0, 0},
      {"halant-deva", 1, 1},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }
  {
    const hb_codepoint_t s[] = {CP_RA, CP_H, CP_I};
    const glyph_expect_t e[] = {
      {"iMatra-deva",  2, 1},
      {"dottedCircle", 0, 0},
      {"reph-deva",    0, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }
  test_close_font (font, face);
}

/* ZWNJ asks for the non-ligated, halant-shown form while ZWJ asks for the
 * joined conjunct; the shaping result must keep that distinction and the
 * control character's source cluster.  Mirrors indic-joiners.tests. */
static void
test_indic_joiner_intent (void)
{
  hb_face_t *face;
  hb_font_t *font = test_open_font (JOINER_FONT, &face);

  /* TTA + Halant + ZWNJ + YA + I: visible halant is retained, the ZWNJ
   * itself stays traceable at source index 2, and the I-matra variant
   * lands before YA (cluster 3) which it attaches to. */
  {
    const hb_codepoint_t s[] = {CP_TTA, CP_H, CP_ZWNJ, CP_YA, CP_I};
    const glyph_expect_t e[] = {
      {"uni091F",    0, 0},
      {"uni094D",    1, 1},
      {"space",      2, 1},
      {"uni093F.750", 4, 1},
      {"uni092F",    3, 0},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  /* TTA + Halant + ZWJ + YA + I: the pre-base vowel moves to the very
   * front of the joined conjunct and every glyph shares cluster 0 at the
   * monotone-characters level (one conjunct unit). */
  {
    const hb_codepoint_t s[] = {CP_TTA, CP_H, CP_ZWJ, CP_YA, CP_I};
    const glyph_expect_t e[] = {
      {"uni093F", 0, -1},
      {"uni091F", 0, -1},
      {"uni094D", 0, -1},
      {"space",   0, -1},
      {"uni092F", 0, -1},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }
  /* At full-character clusters the same ZWJ run keeps the reordered
   * source indices; the vowel is still the first displayed glyph. */
  {
    const hb_codepoint_t s[] = {CP_TTA, CP_H, CP_ZWJ, CP_YA, CP_I};
    const glyph_expect_t e[] = {
      {"uni093F", 4, 1},
      {"uni091F", 0, 0},
      {"uni094D", 1, 1},
      {"space",   2, 1},
      {"uni092F", 3, 1},
    };
    shape_expect (font, s, G_N_ELEMENTS (s),
		  HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		  e, G_N_ELEMENTS (e));
  }

  test_close_font (font, face);
}

/* Line breaking: library-reported safe cuts in original codepoint indices
 * must split the text into fragments whose independent shaping concatenates
 * back to the full run.  Cuts inside a reordering syllable are not safe.
 * The first "line" here is the broken RA+Halant+I syllable that needs an
 * inserted dotted base; that circle must stay in the first fragment. */
static void
test_indic_line_break_reconstruction (void)
{
  hb_face_t *face;
  hb_font_t *font = test_open_font (NOTO_DEVA_FONT, &face);

  /* RA H I SPACE KA I  == "र्ि कि" */
  const hb_codepoint_t text[] = {
    CP_RA, CP_H, CP_I, CP_SPACE, CP_KA, CP_I
  };
  const unsigned int n = G_N_ELEMENTS (text);

  hb_buffer_t *full = shape_fragment (font, text, n, 0, n);

  unsigned int cuts[16];
  unsigned int n_cuts = collect_safe_cuts (full, cuts, G_N_ELEMENTS (cuts));
  g_assert_cmpuint (n_cuts, ==, 2);
  g_assert_cmpuint (cuts[0], ==, 3); /* before the space / after र्ि */
  g_assert_cmpuint (cuts[1], ==, 4); /* after the space / before कि */

  /* Exhaustively check every original cut: only the library's safe cuts
   * may reconstruct the full shaping exactly.  This also proves that the
   * other (cluster-number-different) boundaries are not breakable. */
  for (unsigned int cut = 1; cut < n; cut++)
  {
    hb_buffer_t *head = shape_fragment (font, text, n, 0, cut);
    hb_buffer_t *tail = shape_fragment (font, text, n, cut, n - cut);

    hb_buffer_t *reconstructed = hb_buffer_create ();
    hb_buffer_set_cluster_level (reconstructed,
				 HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_buffer_append (reconstructed, head, 0, -1);
    hb_buffer_append (reconstructed, tail, 0, -1);

    hb_buffer_diff_flags_t diff =
      hb_buffer_diff (reconstructed, full, (hb_codepoint_t) -1, 0);
    hb_buffer_diff_flags_t meaningful =
      (hb_buffer_diff_flags_t) (diff & ~HB_BUFFER_DIFF_FLAG_GLYPH_FLAGS_MISMATCH);

    if (cut == 3 || cut == 4)
      g_assert_cmpuint (meaningful, ==, HB_BUFFER_DIFF_FLAG_EQUAL);
    else
      g_assert_true (meaningful != HB_BUFFER_DIFF_FLAG_EQUAL);

    hb_buffer_destroy (reconstructed);
    hb_buffer_destroy (tail);
    hb_buffer_destroy (head);
  }

  /* Break at codepoint 3: the broken syllable is line one. */
  hb_buffer_t *head = shape_fragment (font, text, n, 0, 3);
  hb_buffer_t *tail = shape_fragment (font, text, n, 3, n - 3);

  /* The first fragment displays I-matra, its inserted dotted-circle base
   * and the Reph formed on that base.  Exactly one circle, belonging to
   * source range [0,3).  (Noto names the dotted circle glyph uni25CC.) */
  g_assert_cmpuint (count_named_glyphs (font, head, "uni25CC"), ==, 1);
  {
    unsigned int len;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos (head, &len);
    g_assert_cmpuint (len, ==, 3);
    g_assert_cmpuint (info[0].cluster, ==, 0); /* ivowelsigndeva */
    g_assert_cmpuint (info[1].cluster, ==, 0); /* dottedCircle */
    g_assert_cmpuint (info[2].cluster, ==, 0); /* rephdeva */
    for (unsigned int i = 0; i < len; i++)
      g_assert_true (info[i].cluster < 3);
  }

  /* The second fragment (" कि") gets no dotted circle attributed to it and
   * its कि shapes on its own as pre-base matra + KA. */
  g_assert_cmpuint (count_named_glyphs (font, tail, "uni25CC"), ==, 0);
  g_assert_cmpuint (count_named_glyphs (font, tail, "dottedCircle"), ==, 0);
  {
    unsigned int len;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos (tail, &len);
    /* space, matra, KA */
    g_assert_cmpuint (len, ==, 3);
    g_assert_cmpuint (info[0].cluster, ==, 3);
    g_assert_cmpuint (info[1].cluster, ==, 4);
    g_assert_cmpuint (info[2].cluster, ==, 4);
    g_assert_true (info[1].cluster >= 3 && info[1].cluster < n);
  }

  /* The standalone following syllable कि gives the same two glyph ids in
   * the same display order, proving it is independently shapable. */
  {
    const hb_codepoint_t ki[] = {CP_KA, CP_I};
    hb_buffer_t *alone = hb_buffer_create ();
    hb_buffer_add_codepoints (alone, ki, 2, 0, 2);
    hb_buffer_set_direction (alone, HB_DIRECTION_LTR);
    hb_buffer_set_script (alone, HB_SCRIPT_DEVANAGARI);
    hb_buffer_set_language (alone, hb_language_from_string ("hi", -1));
    hb_shape (font, alone, NULL, 0);

    unsigned int n_alone, n_tail;
    hb_glyph_info_t *a = hb_buffer_get_glyph_infos (alone, &n_alone);
    hb_glyph_info_t *t = hb_buffer_get_glyph_infos (tail, &n_tail);
    g_assert_cmpuint (n_alone, ==, 2);
    g_assert_cmpuint (n_tail, ==, 3);
    g_assert_cmpuint (a[0].codepoint, ==, t[1].codepoint); /* matra */
    g_assert_cmpuint (a[1].codepoint, ==, t[2].codepoint); /* KA */
    hb_buffer_destroy (alone);
  }

  hb_buffer_destroy (tail);
  hb_buffer_destroy (head);
  hb_buffer_destroy (full);
  test_close_font (font, face);
}

/* Two adjacent clusters can have different cluster numbers without the
 * boundary being safe to break at.  The unsafe-to-break flag, not cluster
 * inequality, is what vetoes such a cut. */
static void
test_indic_cluster_change_is_not_break_safety (void)
{
  hb_face_t *face;
  hb_font_t *font = test_open_font (NOTO_DEVA_FONT, &face);

  /* KA I KA I = "किकि": the second syllable starts at codepoint 2. */
  const hb_codepoint_t text[] = {CP_KA, CP_I, CP_KA, CP_I};
  hb_buffer_t *buffer = shape_fragment (font, text, G_N_ELEMENTS (text),
					0, G_N_ELEMENTS (text));

  unsigned int len;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buffer, &len);

  /* Locate the first glyph carrying cluster 2; it must be unsafe. */
  gboolean found_cluster_two = FALSE;
  for (unsigned int i = 0; i < len; i++)
  {
    if (info[i].cluster == 2)
    {
      g_assert_true (hb_glyph_info_get_glyph_flags (&info[i]) &
		     HB_GLYPH_FLAG_UNSAFE_TO_BREAK);
      found_cluster_two = TRUE;
      break;
    }
  }
  g_assert_true (found_cluster_two);

  unsigned int cuts[16];
  unsigned int n_cuts = collect_safe_cuts (buffer, cuts, G_N_ELEMENTS (cuts));
  for (unsigned int i = 0; i < n_cuts; i++)
    g_assert_cmpuint (cuts[i], !=, 2);

  hb_buffer_destroy (buffer);
  test_close_font (font, face);
}

/* A vowel sign typed without any base must still yield visible glyphs that
 * map back to that original segment: HarfBuzz supplies a dotted circle as
 * the missing base, with the pre-base vowel in front of it. */
static void
test_indic_baseless_vowel_is_displayable (void)
{
  hb_face_t *face;
  hb_font_t *font = test_open_font (NOTO_DEVA_FONT, &face);

  const hb_codepoint_t s[] = {CP_I};
  const glyph_expect_t e[] = {
    {"ivowelsigndeva", 0, -1},
    {"uni25CC",        0, -1},
  };
  shape_expect (font, s, G_N_ELEMENTS (s),
		HB_BUFFER_CLUSTER_LEVEL_CHARACTERS, NULL,
		e, G_N_ELEMENTS (e));

  test_close_font (font, face);
}

int
main (int argc, char **argv)
{
  hb_test_init (&argc, &argv);

  hb_test_add (test_indic_prebase_vowel_editing);
  hb_test_add (test_indic_reph_editing);
  hb_test_add (test_indic_joiner_intent);
  hb_test_add (test_indic_line_break_reconstruction);
  hb_test_add (test_indic_cluster_change_is_not_break_safety);
  hb_test_add (test_indic_baseless_vowel_is_displayable);

  return hb_test_run ();
}
