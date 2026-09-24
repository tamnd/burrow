/* unicode: what kind of character a rune is, and its other cases.
 *
 * The questions here are the ones a lexer or a text tool asks all the time. Is
 * this a letter, a digit, a space? What is its upper case? Which script is it
 * from? The answers come from tables of code point ranges, one per category,
 * script and property in the Unicode Character Database, the same tables Go
 * ships with and generated from them, so the two agree on every rune.
 *
 *     unicode_is_letter('x');                  // true
 *     unicode_is(unicode_han, 0x4E16);         // true, 世 is a Han character
 *     unicode_to_upper(0x03B1);                // 0x0391, alpha to Alpha
 *     unicode_special_case_to_upper(unicode_turkish_case, 'i');  // 0x0130
 *
 * A table is a UnicodeRangeTable, and every one Go exports is here under the
 * same name in snake case: unicode_latin, unicode_greek, unicode_lu,
 * unicode_white_space and so on. They are pointers to constant data, so they
 * cost nothing to use and need no setup.
 *
 * Go's maps from a name to a table, Categories, Scripts and the rest, are
 * UnicodeTableMap values here, sorted arrays you look a name up in with
 * unicode_table_map_get.
 *
 * Nothing in this package allocates or fails. A rune outside the valid range,
 * negative or above UNICODE_MAX_RUNE, is in no table and maps to itself.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package unicode */

#ifndef BURROW_UNICODE_H
#define BURROW_UNICODE_H

#include "burrow/core.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest valid code point. */
#define UNICODE_MAX_RUNE ((Rune)0x10FFFF)
/* The rune that stands in for one that is invalid or cannot be shown. */
#define UNICODE_REPLACEMENT_CHAR ((Rune)0xFFFD)
/* The largest ASCII value. */
#define UNICODE_MAX_ASCII ((Rune)0x7F)
/* The largest Latin-1 value. */
#define UNICODE_MAX_LATIN1 ((Rune)0xFF)

/* The cases, as indexes into UnicodeCaseRange.delta and as the first argument
 * to unicode_to. */
#define UNICODE_UPPER_CASE 0
#define UNICODE_LOWER_CASE 1
#define UNICODE_TITLE_CASE 2
#define UNICODE_MAX_CASE 3

/* A delta that is not a delta. In a UnicodeCaseRange, it means the range
 * alternates upper and lower case, starting with upper: the even code points
 * are upper case and the odd ones after them are their lower case. */
#define UNICODE_UPPER_LOWER (UNICODE_MAX_RUNE + 1)

/* A range of 16 bit code points, lo to hi inclusive, taking every stride'th
 * one. */
typedef struct UnicodeRange16 {
    uint16_t lo;
    uint16_t hi;
    uint16_t stride;
} UnicodeRange16;

/* The same for code points that do not fit in 16 bits. */
typedef struct UnicodeRange32 {
    uint32_t lo;
    uint32_t hi;
    uint32_t stride;
} UnicodeRange32;

/* A set of code points, as sorted ranges that do not overlap. The 16 bit
 * ranges come first and all the 32 bit ones are above them. latin_offset is
 * how many of the 16 bit ranges have hi at or below UNICODE_MAX_LATIN1, which
 * lets the ASCII and Latin-1 fast paths skip them. */
typedef struct UnicodeRangeTable {
    const UnicodeRange16 *r16;
    Int r16_len;
    const UnicodeRange32 *r32;
    Int r32_len;
    Int latin_offset;
} UnicodeRangeTable;

/* A range of code points, lo to hi inclusive, and what to add to each one to
 * get its upper, lower and title case, indexed by UNICODE_UPPER_CASE and the
 * rest. A delta can be UNICODE_UPPER_LOWER, described above. */
typedef struct UnicodeCaseRange {
    uint32_t lo;
    uint32_t hi;
    Rune delta[UNICODE_MAX_CASE];
} UnicodeCaseRange;

/* Case mappings for one language, such as Turkish, sorted by lo. Go's type is
 * []CaseRange. */
typedef struct UnicodeSpecialCase {
    const UnicodeCaseRange *p;
    Int len;
} UnicodeSpecialCase;

/* One entry of a map from a name to a table. */
typedef struct UnicodeNamedTable {
    Str name;
    const UnicodeRangeTable *table;
} UnicodeNamedTable;

/* Go's map[string]*RangeTable, as entries sorted by name. You can walk it with
 * an index from 0 to len. */
typedef struct UnicodeTableMap {
    const UnicodeNamedTable *p;
    Int len;
} UnicodeTableMap;

/* One entry of a map from a name to another name. */
typedef struct UnicodeAlias {
    Str name;
    Str target;
} UnicodeAlias;

/* Go's map[string]string, sorted by name. */
typedef struct UnicodeAliasMap {
    const UnicodeAlias *p;
    Int len;
} UnicodeAliasMap;

/* ------------------------------------------------------------- the tables */

/* BEGIN GENERATED DECLARATIONS */

/* The version of Unicode the tables come from. */
#define UNICODE_VERSION "17.0.0"

/* The tables, by the names Go gives them. */

extern const UnicodeRangeTable *const unicode_cc;
extern const UnicodeRangeTable *const unicode_cf;
extern const UnicodeRangeTable *const unicode_cn;
extern const UnicodeRangeTable *const unicode_co;
extern const UnicodeRangeTable *const unicode_cs;
extern const UnicodeRangeTable *const unicode_digit;
extern const UnicodeRangeTable *const unicode_nd;
extern const UnicodeRangeTable *const unicode_lc;
extern const UnicodeRangeTable *const unicode_letter;
extern const UnicodeRangeTable *const unicode_l;
extern const UnicodeRangeTable *const unicode_lm;
extern const UnicodeRangeTable *const unicode_lo;
extern const UnicodeRangeTable *const unicode_lower;
extern const UnicodeRangeTable *const unicode_ll;
extern const UnicodeRangeTable *const unicode_mark;
extern const UnicodeRangeTable *const unicode_m;
extern const UnicodeRangeTable *const unicode_mc;
extern const UnicodeRangeTable *const unicode_me;
extern const UnicodeRangeTable *const unicode_mn;
extern const UnicodeRangeTable *const unicode_nl;
extern const UnicodeRangeTable *const unicode_no;
extern const UnicodeRangeTable *const unicode_number;
extern const UnicodeRangeTable *const unicode_n;
extern const UnicodeRangeTable *const unicode_other;
extern const UnicodeRangeTable *const unicode_c;
extern const UnicodeRangeTable *const unicode_pc;
extern const UnicodeRangeTable *const unicode_pd;
extern const UnicodeRangeTable *const unicode_pe;
extern const UnicodeRangeTable *const unicode_pf;
extern const UnicodeRangeTable *const unicode_pi;
extern const UnicodeRangeTable *const unicode_po;
extern const UnicodeRangeTable *const unicode_ps;
extern const UnicodeRangeTable *const unicode_punct;
extern const UnicodeRangeTable *const unicode_p;
extern const UnicodeRangeTable *const unicode_sc;
extern const UnicodeRangeTable *const unicode_sk;
extern const UnicodeRangeTable *const unicode_sm;
extern const UnicodeRangeTable *const unicode_so;
extern const UnicodeRangeTable *const unicode_space;
extern const UnicodeRangeTable *const unicode_z;
extern const UnicodeRangeTable *const unicode_symbol;
extern const UnicodeRangeTable *const unicode_s;
extern const UnicodeRangeTable *const unicode_title;
extern const UnicodeRangeTable *const unicode_lt;
extern const UnicodeRangeTable *const unicode_upper;
extern const UnicodeRangeTable *const unicode_lu;
extern const UnicodeRangeTable *const unicode_zl;
extern const UnicodeRangeTable *const unicode_zp;
extern const UnicodeRangeTable *const unicode_zs;
extern const UnicodeRangeTable *const unicode_adlam;
extern const UnicodeRangeTable *const unicode_ahom;
extern const UnicodeRangeTable *const unicode_anatolian_hieroglyphs;
extern const UnicodeRangeTable *const unicode_arabic;
extern const UnicodeRangeTable *const unicode_armenian;
extern const UnicodeRangeTable *const unicode_avestan;
extern const UnicodeRangeTable *const unicode_balinese;
extern const UnicodeRangeTable *const unicode_bamum;
extern const UnicodeRangeTable *const unicode_bassa_vah;
extern const UnicodeRangeTable *const unicode_batak;
extern const UnicodeRangeTable *const unicode_bengali;
extern const UnicodeRangeTable *const unicode_beria_erfe;
extern const UnicodeRangeTable *const unicode_bhaiksuki;
extern const UnicodeRangeTable *const unicode_bopomofo;
extern const UnicodeRangeTable *const unicode_brahmi;
extern const UnicodeRangeTable *const unicode_braille;
extern const UnicodeRangeTable *const unicode_buginese;
extern const UnicodeRangeTable *const unicode_buhid;
extern const UnicodeRangeTable *const unicode_canadian_aboriginal;
extern const UnicodeRangeTable *const unicode_carian;
extern const UnicodeRangeTable *const unicode_caucasian_albanian;
extern const UnicodeRangeTable *const unicode_chakma;
extern const UnicodeRangeTable *const unicode_cham;
extern const UnicodeRangeTable *const unicode_cherokee;
extern const UnicodeRangeTable *const unicode_chorasmian;
extern const UnicodeRangeTable *const unicode_common;
extern const UnicodeRangeTable *const unicode_coptic;
extern const UnicodeRangeTable *const unicode_cuneiform;
extern const UnicodeRangeTable *const unicode_cypriot;
extern const UnicodeRangeTable *const unicode_cypro_minoan;
extern const UnicodeRangeTable *const unicode_cyrillic;
extern const UnicodeRangeTable *const unicode_deseret;
extern const UnicodeRangeTable *const unicode_devanagari;
extern const UnicodeRangeTable *const unicode_dives_akuru;
extern const UnicodeRangeTable *const unicode_dogra;
extern const UnicodeRangeTable *const unicode_duployan;
extern const UnicodeRangeTable *const unicode_egyptian_hieroglyphs;
extern const UnicodeRangeTable *const unicode_elbasan;
extern const UnicodeRangeTable *const unicode_elymaic;
extern const UnicodeRangeTable *const unicode_ethiopic;
extern const UnicodeRangeTable *const unicode_garay;
extern const UnicodeRangeTable *const unicode_georgian;
extern const UnicodeRangeTable *const unicode_glagolitic;
extern const UnicodeRangeTable *const unicode_gothic;
extern const UnicodeRangeTable *const unicode_grantha;
extern const UnicodeRangeTable *const unicode_greek;
extern const UnicodeRangeTable *const unicode_gujarati;
extern const UnicodeRangeTable *const unicode_gunjala_gondi;
extern const UnicodeRangeTable *const unicode_gurmukhi;
extern const UnicodeRangeTable *const unicode_gurung_khema;
extern const UnicodeRangeTable *const unicode_han;
extern const UnicodeRangeTable *const unicode_hangul;
extern const UnicodeRangeTable *const unicode_hanifi_rohingya;
extern const UnicodeRangeTable *const unicode_hanunoo;
extern const UnicodeRangeTable *const unicode_hatran;
extern const UnicodeRangeTable *const unicode_hebrew;
extern const UnicodeRangeTable *const unicode_hiragana;
extern const UnicodeRangeTable *const unicode_imperial_aramaic;
extern const UnicodeRangeTable *const unicode_inherited;
extern const UnicodeRangeTable *const unicode_inscriptional_pahlavi;
extern const UnicodeRangeTable *const unicode_inscriptional_parthian;
extern const UnicodeRangeTable *const unicode_javanese;
extern const UnicodeRangeTable *const unicode_kaithi;
extern const UnicodeRangeTable *const unicode_kannada;
extern const UnicodeRangeTable *const unicode_katakana;
extern const UnicodeRangeTable *const unicode_kawi;
extern const UnicodeRangeTable *const unicode_kayah_li;
extern const UnicodeRangeTable *const unicode_kharoshthi;
extern const UnicodeRangeTable *const unicode_khitan_small_script;
extern const UnicodeRangeTable *const unicode_khmer;
extern const UnicodeRangeTable *const unicode_khojki;
extern const UnicodeRangeTable *const unicode_khudawadi;
extern const UnicodeRangeTable *const unicode_kirat_rai;
extern const UnicodeRangeTable *const unicode_lao;
extern const UnicodeRangeTable *const unicode_latin;
extern const UnicodeRangeTable *const unicode_lepcha;
extern const UnicodeRangeTable *const unicode_limbu;
extern const UnicodeRangeTable *const unicode_linear_a;
extern const UnicodeRangeTable *const unicode_linear_b;
extern const UnicodeRangeTable *const unicode_lisu;
extern const UnicodeRangeTable *const unicode_lycian;
extern const UnicodeRangeTable *const unicode_lydian;
extern const UnicodeRangeTable *const unicode_mahajani;
extern const UnicodeRangeTable *const unicode_makasar;
extern const UnicodeRangeTable *const unicode_malayalam;
extern const UnicodeRangeTable *const unicode_mandaic;
extern const UnicodeRangeTable *const unicode_manichaean;
extern const UnicodeRangeTable *const unicode_marchen;
extern const UnicodeRangeTable *const unicode_masaram_gondi;
extern const UnicodeRangeTable *const unicode_medefaidrin;
extern const UnicodeRangeTable *const unicode_meetei_mayek;
extern const UnicodeRangeTable *const unicode_mende_kikakui;
extern const UnicodeRangeTable *const unicode_meroitic_cursive;
extern const UnicodeRangeTable *const unicode_meroitic_hieroglyphs;
extern const UnicodeRangeTable *const unicode_miao;
extern const UnicodeRangeTable *const unicode_modi;
extern const UnicodeRangeTable *const unicode_mongolian;
extern const UnicodeRangeTable *const unicode_mro;
extern const UnicodeRangeTable *const unicode_multani;
extern const UnicodeRangeTable *const unicode_myanmar;
extern const UnicodeRangeTable *const unicode_nabataean;
extern const UnicodeRangeTable *const unicode_nag_mundari;
extern const UnicodeRangeTable *const unicode_nandinagari;
extern const UnicodeRangeTable *const unicode_new_tai_lue;
extern const UnicodeRangeTable *const unicode_newa;
extern const UnicodeRangeTable *const unicode_nko;
extern const UnicodeRangeTable *const unicode_nushu;
extern const UnicodeRangeTable *const unicode_nyiakeng_puachue_hmong;
extern const UnicodeRangeTable *const unicode_ogham;
extern const UnicodeRangeTable *const unicode_ol_chiki;
extern const UnicodeRangeTable *const unicode_ol_onal;
extern const UnicodeRangeTable *const unicode_old_hungarian;
extern const UnicodeRangeTable *const unicode_old_italic;
extern const UnicodeRangeTable *const unicode_old_north_arabian;
extern const UnicodeRangeTable *const unicode_old_permic;
extern const UnicodeRangeTable *const unicode_old_persian;
extern const UnicodeRangeTable *const unicode_old_sogdian;
extern const UnicodeRangeTable *const unicode_old_south_arabian;
extern const UnicodeRangeTable *const unicode_old_turkic;
extern const UnicodeRangeTable *const unicode_old_uyghur;
extern const UnicodeRangeTable *const unicode_oriya;
extern const UnicodeRangeTable *const unicode_osage;
extern const UnicodeRangeTable *const unicode_osmanya;
extern const UnicodeRangeTable *const unicode_pahawh_hmong;
extern const UnicodeRangeTable *const unicode_palmyrene;
extern const UnicodeRangeTable *const unicode_pau_cin_hau;
extern const UnicodeRangeTable *const unicode_phags_pa;
extern const UnicodeRangeTable *const unicode_phoenician;
extern const UnicodeRangeTable *const unicode_psalter_pahlavi;
extern const UnicodeRangeTable *const unicode_rejang;
extern const UnicodeRangeTable *const unicode_runic;
extern const UnicodeRangeTable *const unicode_samaritan;
extern const UnicodeRangeTable *const unicode_saurashtra;
extern const UnicodeRangeTable *const unicode_sharada;
extern const UnicodeRangeTable *const unicode_shavian;
extern const UnicodeRangeTable *const unicode_siddham;
extern const UnicodeRangeTable *const unicode_sidetic;
extern const UnicodeRangeTable *const unicode_sign_writing;
extern const UnicodeRangeTable *const unicode_sinhala;
extern const UnicodeRangeTable *const unicode_sogdian;
extern const UnicodeRangeTable *const unicode_sora_sompeng;
extern const UnicodeRangeTable *const unicode_soyombo;
extern const UnicodeRangeTable *const unicode_sundanese;
extern const UnicodeRangeTable *const unicode_sunuwar;
extern const UnicodeRangeTable *const unicode_syloti_nagri;
extern const UnicodeRangeTable *const unicode_syriac;
extern const UnicodeRangeTable *const unicode_tagalog;
extern const UnicodeRangeTable *const unicode_tagbanwa;
extern const UnicodeRangeTable *const unicode_tai_le;
extern const UnicodeRangeTable *const unicode_tai_tham;
extern const UnicodeRangeTable *const unicode_tai_viet;
extern const UnicodeRangeTable *const unicode_tai_yo;
extern const UnicodeRangeTable *const unicode_takri;
extern const UnicodeRangeTable *const unicode_tamil;
extern const UnicodeRangeTable *const unicode_tangsa;
extern const UnicodeRangeTable *const unicode_tangut;
extern const UnicodeRangeTable *const unicode_telugu;
extern const UnicodeRangeTable *const unicode_thaana;
extern const UnicodeRangeTable *const unicode_thai;
extern const UnicodeRangeTable *const unicode_tibetan;
extern const UnicodeRangeTable *const unicode_tifinagh;
extern const UnicodeRangeTable *const unicode_tirhuta;
extern const UnicodeRangeTable *const unicode_todhri;
extern const UnicodeRangeTable *const unicode_tolong_siki;
extern const UnicodeRangeTable *const unicode_toto;
extern const UnicodeRangeTable *const unicode_tulu_tigalari;
extern const UnicodeRangeTable *const unicode_ugaritic;
extern const UnicodeRangeTable *const unicode_vai;
extern const UnicodeRangeTable *const unicode_vithkuqi;
extern const UnicodeRangeTable *const unicode_wancho;
extern const UnicodeRangeTable *const unicode_warang_citi;
extern const UnicodeRangeTable *const unicode_yezidi;
extern const UnicodeRangeTable *const unicode_yi;
extern const UnicodeRangeTable *const unicode_zanabazar_square;
extern const UnicodeRangeTable *const unicode_ascii_hex_digit;
extern const UnicodeRangeTable *const unicode_bidi_control;
extern const UnicodeRangeTable *const unicode_dash;
extern const UnicodeRangeTable *const unicode_deprecated;
extern const UnicodeRangeTable *const unicode_diacritic;
extern const UnicodeRangeTable *const unicode_extender;
extern const UnicodeRangeTable *const unicode_hex_digit;
extern const UnicodeRangeTable *const unicode_hyphen;
extern const UnicodeRangeTable *const unicode_ids_binary_operator;
extern const UnicodeRangeTable *const unicode_ids_trinary_operator;
extern const UnicodeRangeTable *const unicode_ids_unary_operator;
extern const UnicodeRangeTable *const unicode_id_compat_math_continue;
extern const UnicodeRangeTable *const unicode_id_compat_math_start;
extern const UnicodeRangeTable *const unicode_ideographic;
extern const UnicodeRangeTable *const unicode_join_control;
extern const UnicodeRangeTable *const unicode_logical_order_exception;
extern const UnicodeRangeTable *const unicode_modifier_combining_mark;
extern const UnicodeRangeTable *const unicode_noncharacter_code_point;
extern const UnicodeRangeTable *const unicode_other_alphabetic;
extern const UnicodeRangeTable *const unicode_other_default_ignorable_code_point;
extern const UnicodeRangeTable *const unicode_other_grapheme_extend;
extern const UnicodeRangeTable *const unicode_other_id_continue;
extern const UnicodeRangeTable *const unicode_other_id_start;
extern const UnicodeRangeTable *const unicode_other_lowercase;
extern const UnicodeRangeTable *const unicode_other_math;
extern const UnicodeRangeTable *const unicode_other_uppercase;
extern const UnicodeRangeTable *const unicode_pattern_syntax;
extern const UnicodeRangeTable *const unicode_pattern_white_space;
extern const UnicodeRangeTable *const unicode_prepended_concatenation_mark;
extern const UnicodeRangeTable *const unicode_quotation_mark;
extern const UnicodeRangeTable *const unicode_radical;
extern const UnicodeRangeTable *const unicode_regional_indicator;
extern const UnicodeRangeTable *const unicode_s_term;
extern const UnicodeRangeTable *const unicode_sentence_terminal;
extern const UnicodeRangeTable *const unicode_soft_dotted;
extern const UnicodeRangeTable *const unicode_terminal_punctuation;
extern const UnicodeRangeTable *const unicode_unified_ideograph;
extern const UnicodeRangeTable *const unicode_variation_selector;
extern const UnicodeRangeTable *const unicode_white_space;

/* Categories: the categories, by their one or two letter names. */
extern const UnicodeTableMap unicode_categories;

/* Scripts: the scripts. */
extern const UnicodeTableMap unicode_scripts;

/* Properties: the properties. */
extern const UnicodeTableMap unicode_properties;

/* FoldCategory: for each category, the code points outside it that are equivalent to one inside it under simple case folding. */
extern const UnicodeTableMap unicode_fold_category;

/* FoldScript: for each script, the code points outside it that are equivalent to one inside it under simple case folding. */
extern const UnicodeTableMap unicode_fold_script;

/* CategoryAliases: the long names of the categories, mapped to the short
 * ones that Categories uses. */
extern const UnicodeAliasMap unicode_category_aliases;

/* The case mappings Turkish and Azeri use, which differ from the defaults
 * for the dotted and dotless i. */
extern const UnicodeSpecialCase unicode_turkish_case;
extern const UnicodeSpecialCase unicode_azeri_case;

/* CaseRanges: the table ToUpper, ToLower and ToTitle look in. Go's type for it
 * is []CaseRange, which has the same shape as SpecialCase. */
extern const UnicodeSpecialCase unicode_case_ranges;

/* GraphicRanges: the tables unicode_is_graphic checks, letters, marks, numbers, punctuation, symbols and spaces, as a Slice of pointers to UnicodeRangeTable. */
extern const Slice unicode_graphic_ranges;

/* PrintRanges: the tables unicode_is_print checks, which are the graphic ones without the spaces, as a Slice of pointers to UnicodeRangeTable. */
extern const Slice unicode_print_ranges;

/* END GENERATED DECLARATIONS */

/* ------------------------------------------------------------- lookups */

/* The table called name in m, or NULL if there is none. This is the lookup
 * Go writes as unicode.Scripts["Greek"]. */
BURROW_STATIC(ret) const UnicodeRangeTable *unicode_table_map_get(UnicodeTableMap m,
                                                                  Str name);

/* The name that name is an alias of in m, and whether there was one. */
bool unicode_alias_map_get(UnicodeAliasMap m, Str name, Str *target);

/* ------------------------------------------------------------- membership */

/* Whether r is in the table. */
bool unicode_is(const UnicodeRangeTable *table, Rune r);

/* Whether r is in any of the tables in ranges, a slice of pointers to
 * UnicodeRangeTable. UNICODE_RANGES makes one from a list, and unicode_in_v
 * takes the tables as arguments the way Go's In does:
 *
 *     unicode_in_v(r, unicode_latin, unicode_greek);
 */
bool unicode_in(Rune r, Slice ranges);

/* The same test with the arguments the other way round, which is how Go had it
 * first. */
bool unicode_is_one_of(Slice ranges, Rune r);

/* A Slice over a list of tables, for unicode_in and unicode_is_one_of. It
 * points at a compound literal, so it lasts as long as the enclosing block. */
#define UNICODE_RANGES(...)                                                            \
    slice_from((void *)(const UnicodeRangeTable *[]){__VA_ARGS__},                     \
               (Int)(sizeof((const UnicodeRangeTable *[]){__VA_ARGS__}) /              \
                     sizeof(const UnicodeRangeTable *)),                               \
               (Int)(sizeof((const UnicodeRangeTable *[]){__VA_ARGS__}) /              \
                     sizeof(const UnicodeRangeTable *)),                               \
               TYPE_UNSAFE_POINTER)

#define unicode_in_v(r, ...) unicode_in((r), UNICODE_RANGES(__VA_ARGS__))

/* Whether r is a control character, U+0000 to U+001F or U+007F to U+009F.
 * Other code points in the C category, such as surrogates, are not counted. */
bool unicode_is_control(Rune r);

/* Whether r is a decimal digit, category Nd. */
bool unicode_is_digit(Rune r);

/* Whether r is graphic: a letter, mark, number, punctuation, symbol or space,
 * categories L, M, N, P, S and Zs. */
bool unicode_is_graphic(Rune r);

/* Whether r is a letter, category L. */
bool unicode_is_letter(Rune r);

/* Whether r is a lower case letter. */
bool unicode_is_lower(Rune r);

/* Whether r is a mark, category M. */
bool unicode_is_mark(Rune r);

/* Whether r is a number, category N. */
bool unicode_is_number(Rune r);

/* Whether r is printable in Go's sense: graphic, but with ASCII space the only
 * space character allowed. */
bool unicode_is_print(Rune r);

/* Whether r is punctuation, category P. */
bool unicode_is_punct(Rune r);

/* Whether r is white space. In Latin-1 that is tab, newline, vertical tab,
 * form feed, carriage return, space, U+0085 and U+00A0. Beyond it, the
 * White_Space property decides. */
bool unicode_is_space(Rune r);

/* Whether r is a symbol, category S. */
bool unicode_is_symbol(Rune r);

/* Whether r is a title case letter. */
bool unicode_is_title(Rune r);

/* Whether r is an upper case letter. */
bool unicode_is_upper(Rune r);

/* ------------------------------------------------------------- case mapping */

/* r in the case given by which, UNICODE_UPPER_CASE, UNICODE_LOWER_CASE or
 * UNICODE_TITLE_CASE. Any other value gives UNICODE_REPLACEMENT_CHAR. */
Rune unicode_to(Int which, Rune r);

/* r in lower case. */
Rune unicode_to_lower(Rune r);

/* r in title case. */
Rune unicode_to_title(Rune r);

/* r in upper case. */
Rune unicode_to_upper(Rune r);

/* The next rune after r in its orbit under simple case folding, the set of
 * runes that are equal ignoring case, going round to the smallest after the
 * largest. For 'A' it is 'a', for 'a' it is 'A', and for 'K' it is 'k' and
 * then U+212A, the Kelvin sign, then 'K' again. A rune with no other case, or
 * one that is not valid, gives itself back. */
Rune unicode_simple_fold(Rune r);

/* r in lower case, using special's mappings first and the defaults for a rune
 * special does not cover. */
Rune unicode_special_case_to_lower(UnicodeSpecialCase special, Rune r);

/* r in title case, the same way. */
Rune unicode_special_case_to_title(UnicodeSpecialCase special, Rune r);

/* r in upper case, the same way. */
Rune unicode_special_case_to_upper(UnicodeSpecialCase special, Rune r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_UNICODE_H */
