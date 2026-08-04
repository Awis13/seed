/*
 * skills/ir_codes_tvbgone.h — TV-B-Gone power-off code database
 *
 * SPDX-License-Identifier: CC-BY-SA-2.5
 *
 * THIS FILE IS NOT MIT. The rest of this repository is (see LICENSE); this one
 * file is Creative Commons Attribution-ShareAlike, and the licence travels with
 * it. Full text: seeds/esp32-tembed/LICENSE-CC-BY-SA.
 *
 * Upstream
 * --------
 * Copied from Arduino-TV-B-Gone, file WORLD_IR_CODES.h:
 *   https://github.com/shirriff/Arduino-TV-B-Gone
 *
 * Its notices, reproduced intact:
 *
 *   TV-B-Gone for Arduino version 0.001
 *   Ported to Arduino by Ken Shirriff, Dec 3, 2009
 *   http://arcfn.com
 *
 *   The original code is:
 *   TV-B-Gone Firmware version 1.2
 *    for use with ATtiny85v and v1.2 hardware
 *    (c) Mitch Altman + Limor Fried 2009
 *
 *   Codes captured from Generation 3 TV-B-Gone by Limor Fried & Mitch Altman
 *   table of POWER codes
 *
 * The EU/NA split is Mitch Altman's, with some code by Kevin Timmerman and
 * Damien Good.
 *
 * Licence, and one loose end
 * --------------------------
 * The authors state the terms in the firmware source itself: "Distributed
 * under Creative Commons 2.5 -- Attib & Share Alike" (main.c, TV-B-Gone
 * Firmware v1.2), and the Arduino port's README repeats it. That is CC BY-SA
 * 2.5, and it is what the SPDX line above records.
 *
 * The loose end: Adafruit's TV-B-Gone-kit repository — Limor Fried is a named
 * author — ships the full Creative Commons Attribution-ShareAlike *3.0*
 * Unported text as its licence file for the same firmware. Upstream is
 * inconsistent about the version. Both versions demand the same three things
 * of us, so both are satisfied the same way: attribution kept above, the
 * licence supplied alongside, and the data left under share-alike instead of
 * being absorbed into MIT. The bundled LICENSE-CC-BY-SA carries the 3.0 text
 * verbatim as upstream ships it, and cites the URI for 2.5, which is how
 * CC BY-SA 2.5 section 4(a) allows the licence to be conveyed.
 *
 * Keeping the data in a file of its own, with no logic in it, is what bounds
 * the share-alike obligation to the table and leaves ir.cpp — which reads it —
 * MIT along with everything else.
 *
 * Format, unchanged from upstream
 * ------------------------------
 * Each code is a carrier frequency, a short table of on/off duration pairs in
 * units of 10us, and a bit stream of fixed-width indices into that table, MSB
 * first. Expanding a code to microseconds is done at transmit time; keeping it
 * packed is why 270 codes fit in a few kilobytes of flash. A frequency of 0
 * means the code is sent without a carrier.
 *
 * There is no repeat count and no gap field, because upstream has none: any
 * repetition a protocol needs is already baked into the index stream, and a
 * long "off" entry in the pair table ends the frame. Each code is sent once.
 *
 * Regions are upstream's: NA covers North America, Asia and everything the EU
 * list does not; EU covers Europe, the Middle East, Australia, New Zealand and
 * parts of Africa and South America. 5 codes appear in both lists and are
 * stored once with both bits set.
 *
 * Generated from upstream, not hand-edited. Names match upstream symbols
 * (na000 is code_na000Code), so any entry here can be traced back.
 */

/* Included from ir.cpp, which defines IR_REGION_NA and IR_REGION_EU. */

struct IrTvbCode {
    uint32_t hz;             /* carrier frequency; 0 = send without a carrier */
    const uint16_t *pairs;   /* on/off durations in 10us units, two per entry */
    const uint8_t *bits;     /* packed indices into pairs[], MSB first */
    uint8_t count;           /* on/off pairs to emit */
    uint8_t index_bits;      /* width of one index in bits[] */
    uint8_t regions;         /* IR_REGION_* bitmask */
};

/* --- Pair tables --- */

static const uint16_t tvb_eu000_pairs[] = {
       43,    47,    43,    91,    43,  8324,    88,    47,
      133,   133,   264,    90,   264,    91,
};
static const uint16_t tvb_eu001_pairs[] = {
       47,   265,    51,    54,    51,   108,    51,   263,
       51,  2053,    51, 11647,   100,   109,
};
static const uint16_t tvb_eu002_pairs[] = {
       43,   206,    46,   204,    46,   456,    46,  3488,
};
static const uint16_t tvb_eu004_pairs[] = {
       44,    45,    44,   131,    44,  7462,   346,   176,
      346,   178,
};
static const uint16_t tvb_eu005_pairs[] = {
       24,   190,    25,    80,    25,   190,    25,  4199,
       25,  4799,
};
static const uint16_t tvb_eu006_pairs[] = {
       53,    63,    53,   172,    53,  4472,    54,     0,
      455,   468,
};
static const uint16_t tvb_eu007_pairs[] = {
       50,    54,    50,   159,    50,  2307,   838,   422,
};
static const uint16_t tvb_eu012_pairs[] = {
       46,   206,    46,   459,    46,  3447,
};
static const uint16_t tvb_eu013_pairs[] = {
       53,    59,    53,   171,    53,  2302,   895,   449,
};
static const uint16_t tvb_eu015_pairs[] = {
       53,    54,    53,   156,    53,  2542,   851,   425,
      853,   424,
};
static const uint16_t tvb_eu016_pairs[] = {
       28,    92,    28,   213,    28,   214,    28,  2771,
};
static const uint16_t tvb_eu017_pairs[] = {
       15,   844,    16,   557,    16,   844,    16,  5224,
};
static const uint16_t tvb_eu019_pairs[] = {
       50,    54,    50,   158,    50,   418,    50,  2443,
      843,   418,
};
static const uint16_t tvb_eu020_pairs[] = {
       48,   301,    48,   651,    48,  1001,    48,  3001,
};
static const uint16_t tvb_eu025_pairs[] = {
       49,    52,    49,   102,    49,   250,    49,   252,
       49,  2377,    49, 12009,   100,    52,   100,   102,
};
static const uint16_t tvb_eu026_pairs[] = {
       14,   491,    14,   743,    14,  4926,
};
static const uint16_t tvb_eu028_pairs[] = {
       47,   267,    50,    55,    50,   110,    50,   265,
       50,  2055,    50, 12117,   100,    57,
};
static const uint16_t tvb_eu029_pairs[] = {
       50,    50,    50,    99,    50,   251,    50,   252,
       50,  1445,    50, 11014,   102,    49,   102,    98,
};
static const uint16_t tvb_eu031_pairs[] = {
       53,    53,    53,   160,    53,  1697,   838,   422,
};
static const uint16_t tvb_eu032_pairs[] = {
       49,   205,    49,   206,    49,   456,    49,  3690,
};
static const uint16_t tvb_eu033_pairs[] = {
       48,   150,    50,   149,    50,   347,    50,  2936,
};
static const uint16_t tvb_eu037_pairs[] = {
       14,   491,    14,   743,    14,  5178,
};
static const uint16_t tvb_eu038_pairs[] = {
        3,  1002,     3,  1495,     3,  3059,
};
static const uint16_t tvb_eu039_pairs[] = {
       13,   445,    13,   674,    13,   675,    13,  4583,
};
static const uint16_t tvb_eu040_pairs[] = {
       85,    89,    85,   264,    85,  3402,   347,   350,
      348,   350,
};
static const uint16_t tvb_eu041_pairs[] = {
       46,   300,    49,   298,    49,   648,    49,   997,
       49,  3056,
};
static const uint16_t tvb_eu043_pairs[] = {
     1037,  4216,  1040,     0,
};
static const uint16_t tvb_eu045_pairs[] = {
      152,   471,   154,   156,   154,   469,   154,  2947,
};
static const uint16_t tvb_eu046_pairs[] = {
       15,   493,    16,   493,    16,   698,    16,  1414,
};
static const uint16_t tvb_eu047_pairs[] = {
        3,   496,     3,   745,     3,  1488,
};
static const uint16_t tvb_eu049_pairs[] = {
       55,    55,    55,   167,    55,  4577,    55,  9506,
      448,   445,   450,   444,
};
static const uint16_t tvb_eu050_pairs[] = {
       91,    88,    91,   267,    91,  3621,   361,   358,
      361,   359,
};
static const uint16_t tvb_eu051_pairs[] = {
       84,    88,    84,   261,    84,  3360,   347,   347,
      347,   348,
};
static const uint16_t tvb_eu052_pairs[] = {
       16,   838,    17,   558,    17,   839,    17,  6328,
};
static const uint16_t tvb_eu054_pairs[] = {
       49,    53,    49,   104,    49,   262,    49,   264,
       49,  8030,   100,   103,
};
static const uint16_t tvb_eu056_pairs[] = {
      112,   107,   113,   107,   677,  2766,
};
static const uint16_t tvb_eu059_pairs[] = {
      310,   613,   310,   614,   622,  8312,
};
static const uint16_t tvb_eu060_pairs[] = {
       50,   158,    53,    51,    53,   156,    53,  2180,
};
static const uint16_t tvb_eu064_pairs[] = {
       47,   267,    50,    55,    50,   110,    50,   265,
       50,  2055,    50, 12117,   100,    57,   100,   112,
};
static const uint16_t tvb_eu065_pairs[] = {
       47,   267,    50,    55,    50,   110,    50,   265,
       50,  2055,    50, 12117,   100,   112,
};
static const uint16_t tvb_eu067_pairs[] = {
       94,   473,    94,   728,   102,  1637,
};
static const uint16_t tvb_eu068_pairs[] = {
       49,   263,    50,    54,    50,   108,    50,   263,
       50,  2029,    50, 10199,   100,   110,
};
static const uint16_t tvb_eu069_pairs[] = {
        4,   499,     4,   750,     4,  4999,
};
static const uint16_t tvb_eu071_pairs[] = {
       14,   491,    14,   743,    14,  4422,
};
static const uint16_t tvb_eu072_pairs[] = {
        5,   568,     5,   854,     5,  4999,
};
static const uint16_t tvb_eu075_pairs[] = {
        6,   566,     6,   851,     6,  5474,
};
static const uint16_t tvb_eu076_pairs[] = {
       14,   843,    16,   555,    16,   841,    16,  4911,
};
static const uint16_t tvb_eu078_pairs[] = {
        6,   925,     6,  1339,     6,  2098,     6,  2787,
};
static const uint16_t tvb_eu079_pairs[] = {
       53,    59,    53,   170,    53,  4359,   892,   448,
      893,   448,
};
static const uint16_t tvb_eu080_pairs[] = {
       55,    57,    55,   167,    55,  4416,   895,   448,
      897,   447,
};
static const uint16_t tvb_eu081_pairs[] = {
       26,   185,    27,    80,    27,   185,    27,  4249,
};
static const uint16_t tvb_eu082_pairs[] = {
       51,    56,    51,   162,    51,  2842,   848,   430,
      850,   429,
};
static const uint16_t tvb_eu083_pairs[] = {
       16,   559,    16,   847,    16,  5900,    17,   559,
       17,   847,
};
static const uint16_t tvb_eu084_pairs[] = {
       16,   484,    16,   738,    16,   739,    16,  4795,
};
static const uint16_t tvb_eu085_pairs[] = {
       48,    52,    48,   160,    48,   400,    48,  2120,
      799,   400,
};
static const uint16_t tvb_eu086_pairs[] = {
       16,   851,    17,   554,    17,   850,    17,   851,
       17,  4847,
};
static const uint16_t tvb_eu087_pairs[] = {
       14,   491,    14,   743,    14,  5126,
};
static const uint16_t tvb_eu088_pairs[] = {
       14,   491,    14,   743,    14,  4874,
};
static const uint16_t tvb_eu090_pairs[] = {
        3,     9,     3,    19,     3,    29,     3,    39,
        3,  9968,
};
static const uint16_t tvb_eu091_pairs[] = {
       15,   138,    15,   446,    15,   605,    15,  6565,
};
static const uint16_t tvb_eu092_pairs[] = {
       48,    50,    48,   148,    48,   149,    48,  1424,
};
static const uint16_t tvb_eu093_pairs[] = {
       87,   639,    88,   275,    88,   639,
};
static const uint16_t tvb_eu094_pairs[] = {
        3,     8,     3,    18,     3,    24,     3,    38,
        3,  9969,
};
static const uint16_t tvb_eu096_pairs[] = {
       13,   608,    14,   141,    14,   296,    14,   451,
       14,   606,    14,   608,    14,  6207,
};
static const uint16_t tvb_eu098_pairs[] = {
        3,     8,     3,    18,     3,    28,     3, 12731,
};
static const uint16_t tvb_eu099_pairs[] = {
       46,    53,    46,   106,    46,   260,    46,  1502,
       46, 10962,    93,    53,    93,   106,
};
static const uint16_t tvb_eu101_pairs[] = {
       14,   491,    14,   743,    14,  4674,
};
static const uint16_t tvb_eu103_pairs[] = {
       44,   815,    45,   528,    45,   815,    45,  5000,
};
static const uint16_t tvb_eu104_pairs[] = {
       14,   491,    14,   743,    14,  5881,
};
static const uint16_t tvb_eu106_pairs[] = {
       48,   246,    50,    47,    50,    94,    50,   245,
       50,  1488,    50, 10970,   100,    47,   100,    94,
};
static const uint16_t tvb_eu107_pairs[] = {
       16,   847,    16,  5900,    17,   559,    17,   846,
       17,   847,
};
static const uint16_t tvb_eu108_pairs[] = {
       14,   491,    14,   743,    14,  4622,
};
static const uint16_t tvb_eu109_pairs[] = {
       24,   185,    27,    78,    27,   183,    27,  1542,
};
static const uint16_t tvb_eu110_pairs[] = {
       56,    55,    56,   168,    56,  4850,   447,   453,
      448,   453,
};
static const uint16_t tvb_eu111_pairs[] = {
       49,    52,    49,   250,    49,   252,    49,  2377,
       49, 12009,   100,    52,   100,   102,
};
static const uint16_t tvb_eu112_pairs[] = {
       55,    55,    55,   167,    55,  5023,    55,  9506,
      448,   445,   450,   444,
};
static const uint16_t tvb_eu115_pairs[] = {
       48,    98,    48,   196,    97,   836,   395,   388,
     1931,   389,
};
static const uint16_t tvb_eu116_pairs[] = {
        3,     9,     3,    31,     3,    42,     3, 10957,
};
static const uint16_t tvb_eu117_pairs[] = {
       49,    53,    49,   262,    49,   264,    49,  8030,
      100,   103,
};
static const uint16_t tvb_eu118_pairs[] = {
       44,   815,    45,   528,    45,   815,    45,  4713,
};
static const uint16_t tvb_eu119_pairs[] = {
       14,   491,    14,   743,    14,  5430,
};
static const uint16_t tvb_eu120_pairs[] = {
       19,    78,    21,    27,    21,    77,    21,  3785,
       22,     0,
};
static const uint16_t tvb_eu123_pairs[] = {
       13,   490,    13,   741,    13,   742,    13,  5443,
};
static const uint16_t tvb_eu124_pairs[] = {
       50,    54,    50,   158,    50,   407,    50,  2153,
      843,   407,
};
static const uint16_t tvb_eu125_pairs[] = {
       55,    56,    55,   168,    55,  3929,    56,     0,
      882,   454,   884,   452,
};
static const uint16_t tvb_eu128_pairs[] = {
      152,   471,   154,   156,   154,   469,   154,   782,
      154,  2947,
};
static const uint16_t tvb_eu129_pairs[] = {
       50,    50,    50,    99,    50,   251,    50,   252,
       50,  1449,    50, 11014,   102,    49,   102,    98,
};
static const uint16_t tvb_eu131_pairs[] = {
       14,   491,    14,   743,    14,  4170,
};
static const uint16_t tvb_eu134_pairs[] = {
       13,   490,    13,   741,    13,   742,    13,  5939,
};
static const uint16_t tvb_eu135_pairs[] = {
        6,   566,     6,   851,     6,  5188,
};
static const uint16_t tvb_eu137_pairs[] = {
       86,    91,    87,    90,    87,   180,    87,  8868,
       88,     0,   174,    90,
};
static const uint16_t tvb_eu138_pairs[] = {
        4,  1036,     4,  1507,     4,  3005,
};
static const uint16_t tvb_eu139_pairs[] = {
        0,     0,    14,   141,    14,   452,    14,   607,
       14,  6310,
};
static const uint16_t tvb_na000_pairs[] = {
       60,    60,    60,  2700,   120,    60,   240,    60,
};
static const uint16_t tvb_na001_pairs[] = {
       50,   100,    50,   200,    50,   800,   400,   400,
};
static const uint16_t tvb_na002_pairs[] = {
       42,    46,    42,   133,    42,  7519,   347,   176,
      347,   177,
};
static const uint16_t tvb_na003_pairs[] = {
       26,   185,    27,    80,    27,   185,    27,  4549,
};
static const uint16_t tvb_na004_pairs[] = {
       55,    57,    55,   170,    55,  3949,    55,  9623,
       56,     0,   898,   453,   900,   226,
};
static const uint16_t tvb_na005_pairs[] = {
       88,    90,    88,    91,    88,   181,    88,  8976,
      177,    91,
};
static const uint16_t tvb_na006_pairs[] = {
       50,    62,    50,   172,    50,  4541,   448,   466,
      450,   465,
};
static const uint16_t tvb_na007_pairs[] = {
       49,    49,    49,    50,    49,   410,    49,   510,
       49, 12107,
};
static const uint16_t tvb_na008_pairs[] = {
       56,    58,    56,   170,    56,  4011,   898,   450,
      900,   449,
};
static const uint16_t tvb_na009_pairs[] = {
       53,    56,    53,   171,    53,  3950,    53,  9599,
      898,   451,   900,   226,
};
static const uint16_t tvb_na010_pairs[] = {
       51,    55,    51,   158,    51,  2286,   841,   419,
};
static const uint16_t tvb_na011_pairs[] = {
       55,    55,    55,   172,    55,  4039,    55,  9348,
       56,     0,   884,   442,   885,   225,
};
static const uint16_t tvb_na012_pairs[] = {
       81,    87,    81,   254,    81,  3280,   331,   336,
      331,   337,
};
static const uint16_t tvb_na013_pairs[] = {
       53,    55,    53,   167,    53,  2304,    53,  9369,
      893,   448,   895,   447,
};
static const uint16_t tvb_na016_pairs[] = {
       28,    90,    28,   211,    28,  2507,
};
static const uint16_t tvb_na017_pairs[] = {
       56,    57,    56,   175,    56,  4150,    56,  9499,
      898,   227,   898,   449,
};
static const uint16_t tvb_na018_pairs[] = {
       51,    55,    51,   161,    51,  2566,   849,   429,
      849,   430,
};
static const uint16_t tvb_na019_pairs[] = {
       40,    42,    40,   124,    40,  4601,   325,   163,
      326,   163,
};
static const uint16_t tvb_na020_pairs[] = {
       60,    55,    60,   163,    60,  4099,    60,  9698,
       61,     0,   898,   461,   900,   230,
};
static const uint16_t tvb_na021_pairs[] = {
       48,    52,    48,   160,    48,   400,    48,  2335,
      799,   400,
};
static const uint16_t tvb_na022_pairs[] = {
       53,    60,    53,   175,    53,  4463,    53,  9453,
      892,   450,   895,   225,
};
static const uint16_t tvb_na023_pairs[] = {
       48,    52,    48,   409,    48,   504,    48, 10461,
};
static const uint16_t tvb_na024_pairs[] = {
       58,    60,    58,  2569,   118,    60,   237,    60,
      238,    60,
};
static const uint16_t tvb_na025_pairs[] = {
       84,    90,    84,   264,    84,  3470,   346,   350,
      347,   350,
};
static const uint16_t tvb_na026_pairs[] = {
       49,    49,    49,    50,    49,   410,    49,   510,
       49, 12582,
};
static const uint16_t tvb_na028_pairs[] = {
      118,   121,   118,   271,   118,  4750,   258,   271,
};
static const uint16_t tvb_na029_pairs[] = {
       88,    90,    88,    91,    88,   181,   177,    91,
      177,  8976,
};
static const uint16_t tvb_na031_pairs[] = {
       88,    89,    88,    90,    88,   179,    88,  8977,
      177,    90,
};
static const uint16_t tvb_na033_pairs[] = {
       40,    43,    40,   122,    40,  5297,   334,   156,
      336,   155,
};
static const uint16_t tvb_na035_pairs[] = {
       96,    93,    97,    93,    97,   287,    97,  3431,
};
static const uint16_t tvb_na036_pairs[] = {
       82,   581,    84,   250,    84,   580,    85,     0,
};
static const uint16_t tvb_na037_pairs[] = {
       39,   263,   164,   163,   514,   164,
};
static const uint16_t tvb_na039_pairs[] = {
      113,   101,   688,  2707,
};
static const uint16_t tvb_na040_pairs[] = {
      113,   101,   113,   201,   113,  2707,
};
static const uint16_t tvb_na041_pairs[] = {
       58,    62,    58,  2746,   117,    62,   242,    62,
};
static const uint16_t tvb_na042_pairs[] = {
       54,    65,    54,   170,    54,  4099,    54,  8668,
      899,   226,   899,   421,
};
static const uint16_t tvb_na043_pairs[] = {
       43,   120,    43,   121,    43,  3491,   131,    45,
};
static const uint16_t tvb_na044_pairs[] = {
       51,    51,    51,   160,    51,  4096,    51,  9513,
      431,   436,   883,   219,
};
static const uint16_t tvb_na045_pairs[] = {
       58,    53,    58,   167,    58,  4494,    58,  9679,
      455,   449,   456,   449,
};
static const uint16_t tvb_na046_pairs[] = {
       51,   277,    52,    53,    52,   105,    52,   277,
       52,  2527,    52, 12809,   103,    54,
};
static const uint16_t tvb_na049_pairs[] = {
      274,   854,   274,  1986,
};
static const uint16_t tvb_na050_pairs[] = {
       80,    88,    80,   254,    80,  3750,   359,   331,
};
static const uint16_t tvb_na053_pairs[] = {
       51,   232,    51,   512,    51,   792,    51,  2883,
};
static const uint16_t tvb_na055_pairs[] = {
        3,    10,     3,    20,     3,    30,     3, 12778,
};
static const uint16_t tvb_na056_pairs[] = {
       55,   193,    57,   192,    57,   384,    58,     0,
};
static const uint16_t tvb_na057_pairs[] = {
       45,   148,    46,   148,    46,   351,    46,  2781,
};
static const uint16_t tvb_na058_pairs[] = {
       22,   101,    22,   219,    23,   101,    23,   219,
       31,   218,
};
static const uint16_t tvb_na065_pairs[] = {
       48,    98,    48,   197,    98,   846,   395,   392,
     1953,   392,
};
static const uint16_t tvb_na066_pairs[] = {
       38,   276,   165,   154,   415,   155,   742,   154,
};
static const uint16_t tvb_na068_pairs[] = {
       43,   121,    43,  9437,   130,    45,   131,    45,
};
static const uint16_t tvb_na070_pairs[] = {
       27,    76,    27,   182,    27,   183,    27,  3199,
};
static const uint16_t tvb_na071_pairs[] = {
       37,   181,    37,   272,
};
static const uint16_t tvb_na075_pairs[] = {
       51,    98,    51,   194,   102,   931,   390,   390,
      390,   391,
};
static const uint16_t tvb_na078_pairs[] = {
       40,   275,   160,   154,   480,   155,
};
static const uint16_t tvb_na081_pairs[] = {
       48,    52,    48,   409,    48,   504,    48,  9978,
};
static const uint16_t tvb_na082_pairs[] = {
       88,    89,    88,    90,    88,   179,    88,  8888,
      177,    90,   177,   179,
};
static const uint16_t tvb_na084_pairs[] = {
       41,    43,    41,   128,    41,  7476,   336,   171,
      338,   169,
};
static const uint16_t tvb_na085_pairs[] = {
       55,    60,    55,   165,    55,  2284,   445,   437,
      448,   436,
};
static const uint16_t tvb_na086_pairs[] = {
       42,    46,    42,   126,    42,  6989,   347,   176,
      347,   177,
};
static const uint16_t tvb_na087_pairs[] = {
       56,    69,    56,   174,    56,  4165,    56,  9585,
      880,   222,   880,   435,
};
static const uint16_t tvb_na090_pairs[] = {
       88,    90,    88,    91,    88,   181,    88,  8976,
      177,    91,   177,   181,
};
static const uint16_t tvb_na091_pairs[] = {
       48,   100,    48,   200,    48,  1050,   400,   400,
};
static const uint16_t tvb_na092_pairs[] = {
       54,    56,    54,   170,    54,  4927,   451,   447,
};
static const uint16_t tvb_na093_pairs[] = {
       55,    57,    55,   167,    55,  4400,   895,   448,
      897,   447,
};
static const uint16_t tvb_na095_pairs[] = {
       56,    58,    56,   174,    56,  4549,    56,  9448,
      440,   446,
};
static const uint16_t tvb_na100_pairs[] = {
       43,   171,    45,    60,    45,   170,    54,  2301,
};
static const uint16_t tvb_na102_pairs[] = {
       86,    87,    86,   258,    86,  3338,   346,   348,
      348,   347,
};
static const uint16_t tvb_na109_pairs[] = {
       58,    61,    58,   211,    58,  9582,    73,  4164,
      883,   211,  1050,   494,
};
static const uint16_t tvb_na113_pairs[] = {
       56,    54,    56,   166,    56,  3945,   896,   442,
      896,   443,
};
static const uint16_t tvb_na114_pairs[] = {
       44,    50,    44,   147,    44,   447,    44,  2236,
      791,   398,   793,   397,
};
static const uint16_t tvb_na115_pairs[] = {
       81,    86,    81,   296,    81,  3349,   328,   331,
      329,   331,
};
static const uint16_t tvb_na117_pairs[] = {
       49,    54,    49,   158,    49,   420,    49,  2446,
      819,   420,   821,   419,
};
static const uint16_t tvb_na119_pairs[] = {
       55,    63,    55,   171,    55,  4094,    55,  9508,
      881,   219,   881,   438,
};
static const uint16_t tvb_na122_pairs[] = {
       80,    95,    80,   249,    80,  3867,    81,     0,
      329,   322,
};
static const uint16_t tvb_na124_pairs[] = {
       54,    56,    54,   151,    54,  4092,    54,  8677,
      900,   421,   901,   226,
};
static const uint16_t tvb_na127_pairs[] = {
      114,   100,   115,   100,   115,   200,   115,  2706,
};
static const uint16_t tvb_na130_pairs[] = {
       88,    90,    88,   258,    88,  2247,   358,   349,
      358,   350,
};
static const uint16_t tvb_na132_pairs[] = {
       28,   106,    28,   238,    28,   370,    28,  1173,
};
static const uint16_t tvb_na133_pairs[] = {
       13,   741,    15,   489,    15,   740,    17,  4641,
       18,     0,
};
static const uint16_t tvb_na135_pairs[] = {
       53,    59,    53,   171,    53,  2301,   892,   450,
      895,   448,
};
static const uint16_t tvb_na136_pairs[] = {
       53,    59,    53,   171,    53,  2301,    55,     0,
      892,   450,   895,   448,
};

/* --- Index streams --- */

static const uint8_t tvb_na000_bits[] = {
    0xE2, 0x20, 0x80, 0x78, 0x88, 0x20, 0x10,
};
static const uint8_t tvb_na001_bits[] = {
    0xD5, 0x41, 0x11, 0x00, 0x14, 0x44, 0x6D, 0x54, 0x11, 0x10, 0x01, 0x44,
    0x45,
};
static const uint8_t tvb_na002_bits[] = {
    0x60, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x04, 0x12, 0x48, 0x04, 0x12, 0x48, 0x2A, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x10, 0x49, 0x20, 0x10, 0x49,
    0x20, 0x80,
};
static const uint8_t tvb_na003_bits[] = {
    0x15, 0x5A, 0x65, 0x67, 0x95, 0x65, 0x9A, 0x9B, 0x95, 0x5A, 0x65, 0x67,
    0x95, 0x65, 0x9A, 0x99,
};
static const uint8_t tvb_na004_bits[] = {
    0xA0, 0x00, 0x01, 0x04, 0x92, 0x48, 0x20, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na005_bits[] = {
    0x10, 0x92, 0x49, 0x46, 0x33, 0x09, 0x24, 0x94, 0x60,
};
static const uint8_t tvb_na006_bits[] = {
    0x64, 0x90, 0x00, 0x04, 0x90, 0x00, 0x00, 0x80, 0x00, 0x04, 0x12, 0x49,
    0x2A, 0x12, 0x40, 0x00, 0x12, 0x40, 0x00, 0x02, 0x00, 0x00, 0x10, 0x49,
    0x24, 0x90,
};
static const uint8_t tvb_na007_bits[] = {
    0x09, 0x94, 0x53, 0x29, 0x94, 0xD9, 0x85, 0x32, 0x8A, 0x65, 0x32, 0x9B,
    0x20,
};
static const uint8_t tvb_na008_bits[] = {
    0x64, 0x00, 0x49, 0x00, 0x92, 0x00, 0x20, 0x82, 0x01, 0x04, 0x10, 0x48,
    0x2A, 0x10, 0x01, 0x24, 0x02, 0x48, 0x00, 0x82, 0x08, 0x04, 0x10, 0x41,
    0x20, 0x90,
};
static const uint8_t tvb_na009_bits[] = {
    0x84, 0x90, 0x00, 0x20, 0x80, 0x08, 0x00, 0x00, 0x09, 0x24, 0x92, 0x40,
    0x0A, 0xBA, 0x40,
};
static const uint8_t tvb_na010_bits[] = {
    0xD4, 0x00, 0x15, 0x10, 0x25, 0x00, 0x05, 0x44, 0x09, 0x40, 0x01, 0x51,
    0x01,
};
static const uint8_t tvb_na011_bits[] = {
    0xA0, 0x00, 0x41, 0x04, 0x92, 0x08, 0x24, 0x90, 0x40, 0x00, 0x02, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na012_bits[] = {
    0x64, 0x12, 0x08, 0x24, 0x00, 0x08, 0x20, 0x10, 0x09, 0x2A, 0x10, 0x48,
    0x20, 0x90, 0x00, 0x20, 0x80, 0x40, 0x24, 0x90,
};
static const uint8_t tvb_na013_bits[] = {
    0x80, 0x12, 0x40, 0x04, 0x00, 0x09, 0x00, 0x12, 0x41, 0x24, 0x82, 0x01,
    0x00, 0x10, 0x48, 0x24, 0xAA, 0xE8,
};
static const uint8_t tvb_na014_bits[] = {
    0xA0, 0x00, 0x09, 0x04, 0x92, 0x40, 0x24, 0x80, 0x00, 0x00, 0x12, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na015_bits[] = {
    0xA0, 0x80, 0x01, 0x04, 0x12, 0x48, 0x24, 0x00, 0x00, 0x00, 0x92, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na016_bits[] = {
    0x54, 0x04, 0x10, 0x00, 0x95, 0x01, 0x04, 0x00, 0x10,
};
static const uint8_t tvb_na017_bits[] = {
    0xA0, 0x02, 0x48, 0x04, 0x90, 0x01, 0x20, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na018_bits[] = {
    0x60, 0x82, 0x08, 0x24, 0x10, 0x41, 0x00, 0x12, 0x40, 0x04, 0x80, 0x09,
    0x2A, 0x02, 0x08, 0x20, 0x90, 0x41, 0x04, 0x00, 0x49, 0x00, 0x12, 0x00,
    0x24, 0xA8, 0x08, 0x20, 0x82, 0x41, 0x04, 0x10, 0x01, 0x24, 0x00, 0x48,
    0x00, 0x92, 0xA0, 0x20, 0x82, 0x09, 0x04, 0x10, 0x40, 0x04, 0x90, 0x01,
    0x20, 0x02, 0x48,
};
static const uint8_t tvb_na019_bits[] = {
    0x60, 0x10, 0x40, 0x04, 0x80, 0x09, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x20, 0x10, 0x00, 0x20, 0x80, 0x00, 0x0A, 0x00, 0x41, 0x00, 0x12, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0x40, 0x00, 0x82, 0x00,
    0x00, 0x00,
};
static const uint8_t tvb_na020_bits[] = {
    0xA0, 0x10, 0x00, 0x04, 0x82, 0x49, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na021_bits[] = {
    0x80, 0x10, 0x40, 0x08, 0x82, 0x08, 0x01, 0xC0, 0x08, 0x20, 0x04, 0x41,
    0x04, 0x00, 0x00,
};
static const uint8_t tvb_na022_bits[] = {
    0x80, 0x02, 0x40, 0x00, 0x02, 0x40, 0x00, 0x00, 0x01, 0x24, 0x92, 0x48,
    0x0A, 0xBA, 0x00,
};
static const uint8_t tvb_na023_bits[] = {
    0xA1, 0x18, 0x61, 0xA1, 0x18, 0x7A, 0x11, 0x86, 0x1A, 0x11, 0x86,
};
static const uint8_t tvb_na024_bits[] = {
    0x69, 0x24, 0x10, 0x40, 0x03, 0x12, 0x48, 0x20, 0x80, 0x00,
};
static const uint8_t tvb_na025_bits[] = {
    0x64, 0x92, 0x49, 0x00, 0x00, 0x00, 0x00, 0x02, 0x49, 0x2A, 0x12, 0x49,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x09, 0x24, 0x90,
};
static const uint8_t tvb_na026_bits[] = {
    0x09, 0x94, 0x53, 0x65, 0x32, 0x99, 0x85, 0x32, 0x8A, 0x6C, 0xA6, 0x53,
    0x20,
};
static const uint8_t tvb_na027_bits[] = {
    0xC5, 0x41, 0x11, 0x10, 0x14, 0x44, 0x6C, 0x54, 0x11, 0x11, 0x01, 0x44,
    0x44,
};
static const uint8_t tvb_na028_bits[] = {
    0xC4, 0x45, 0x14, 0x04, 0x6C, 0x44, 0x51, 0x40, 0x44,
};
static const uint8_t tvb_na029_bits[] = {
    0x0C, 0x92, 0x53, 0x46, 0x16, 0x49, 0x29, 0xA2, 0xC0,
};
static const uint8_t tvb_na030_bits[] = {
    0x80, 0x00, 0x41, 0x04, 0x12, 0x08, 0x20, 0x00, 0x00, 0x04, 0x92, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na031_bits[] = {
    0x06, 0x12, 0x49, 0x46, 0x32, 0x61, 0x24, 0x94, 0x60,
};
static const uint8_t tvb_na032_bits[] = {
    0x80, 0x00, 0x41, 0x04, 0x12, 0x08, 0x20, 0x80, 0x00, 0x04, 0x12, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na033_bits[] = {
    0x60, 0x10, 0x40, 0x04, 0x80, 0x09, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x20, 0x82, 0x00, 0x20, 0x00, 0x00, 0x0A, 0x00, 0x41, 0x00, 0x12, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x82, 0x08, 0x00, 0x80, 0x00,
    0x00, 0x00,
};
static const uint8_t tvb_na034_bits[] = {
    0xA0, 0x00, 0x41, 0x04, 0x92, 0x08, 0x24, 0x92, 0x48, 0x00, 0x00, 0x01,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na035_bits[] = {
    0x16, 0x66, 0x5D, 0x59, 0x99, 0x50,
};
static const uint8_t tvb_na036_bits[] = {
    0x15, 0x9A, 0x9C,
};
static const uint8_t tvb_na037_bits[] = {
    0x80, 0x45, 0x00,
};
static const uint8_t tvb_na038_bits[] = {
    0xA4, 0x10, 0x40, 0x00, 0x82, 0x09, 0x20, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na039_bits[] = {
    0x11,
};
static const uint8_t tvb_na040_bits[] = {
    0x06, 0x04,
};
static const uint8_t tvb_na041_bits[] = {
    0xE2, 0x20, 0x80, 0x78, 0x88, 0x20, 0x00,
};
static const uint8_t tvb_na042_bits[] = {
    0xA4, 0x80, 0x00, 0x20, 0x82, 0x49, 0x00, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na043_bits[] = {
    0x15, 0x75, 0x56, 0x55, 0x75, 0x54,
};
static const uint8_t tvb_na044_bits[] = {
    0x84, 0x90, 0x00, 0x00, 0x02, 0x49, 0x20, 0x80, 0x00, 0x04, 0x12, 0x49,
    0x2A, 0xBA, 0x40,
};
static const uint8_t tvb_na045_bits[] = {
    0x80, 0x90, 0x00, 0x00, 0x90, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x97, 0x48,
};
static const uint8_t tvb_na046_bits[] = {
    0x0B, 0x12, 0x63, 0x44, 0x92, 0x6B, 0x44, 0x92, 0x50,
};
static const uint8_t tvb_na047_bits[] = {
    0xA0, 0x00, 0x40, 0x04, 0x92, 0x09, 0x24, 0x92, 0x09, 0x20, 0x00, 0x40,
    0x0A, 0x38, 0x00,
};
static const uint8_t tvb_na048_bits[] = {
    0x80, 0x00, 0x00, 0x04, 0x92, 0x49, 0x24, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na049_bits[] = {
    0x14, 0x11, 0x40,
};
static const uint8_t tvb_na050_bits[] = {
    0xC0, 0x00, 0x01, 0x55, 0x55, 0x52, 0xC0, 0x00, 0x01, 0x55, 0x55, 0x50,
};
static const uint8_t tvb_na051_bits[] = {
    0xA0, 0x10, 0x01, 0x24, 0x82, 0x48, 0x00, 0x02, 0x40, 0x04, 0x90, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na052_bits[] = {
    0xA4, 0x90, 0x48, 0x00, 0x02, 0x01, 0x20, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na053_bits[] = {
    0x22, 0x21, 0x40, 0x1C, 0x88, 0x85, 0x00, 0x40,
};
static const uint8_t tvb_na054_bits[] = {
    0x22, 0x20, 0x15, 0x72, 0x22, 0x01, 0x54,
};
static const uint8_t tvb_na055_bits[] = {
    0x81, 0x51, 0x14, 0xB8, 0x15, 0x11, 0x44,
};
static const uint8_t tvb_na056_bits[] = {
    0x2A, 0x57,
};
static const uint8_t tvb_na057_bits[] = {
    0x2A, 0x5D, 0xA9, 0x60,
};
static const uint8_t tvb_na058_bits[] = {
    0x8D, 0xA4, 0x08, 0x04, 0x04, 0x92, 0x4C,
};
static const uint8_t tvb_na059_bits[] = {
    0xA4, 0x12, 0x09, 0x00, 0x80, 0x40, 0x20, 0x10, 0x40, 0x04, 0x82, 0x09,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na060_bits[] = {
    0xA0, 0x00, 0x08, 0x04, 0x92, 0x41, 0x24, 0x00, 0x40, 0x00, 0x92, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na061_bits[] = {
    0xA0, 0x00, 0x08, 0x24, 0x92, 0x41, 0x04, 0x82, 0x00, 0x00, 0x10, 0x49,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na062_bits[] = {
    0xA0, 0x02, 0x08, 0x04, 0x90, 0x41, 0x24, 0x82, 0x00, 0x00, 0x10, 0x49,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na063_bits[] = {
    0xA4, 0x92, 0x49, 0x20, 0x00, 0x00, 0x04, 0x92, 0x48, 0x00, 0x00, 0x01,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na064_bits[] = {
    0xC0, 0x01, 0x51, 0x55, 0x54, 0x04, 0x2C, 0x00, 0x15, 0x15, 0x55, 0x40,
    0x40,
};
static const uint8_t tvb_na065_bits[] = {
    0x84, 0x92, 0x01, 0x24, 0x12, 0x00, 0x04, 0x80, 0x08, 0x09, 0x92, 0x48,
    0x04, 0x90, 0x48, 0x00, 0x12, 0x00, 0x20, 0x26, 0x49, 0x20, 0x12, 0x41,
    0x20, 0x00, 0x48, 0x00, 0x80, 0x80,
};
static const uint8_t tvb_na066_bits[] = {
    0xC0, 0x45, 0x02, 0x01, 0x14, 0x08, 0x04, 0x50, 0x00,
};
static const uint8_t tvb_na067_bits[] = {
    0x80, 0x02, 0x49, 0x24, 0x90, 0x00, 0x00, 0x80, 0x00, 0x04, 0x12, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na068_bits[] = {
    0x8C, 0x30, 0x0D, 0xCC, 0x30, 0x0C,
};
static const uint8_t tvb_na069_bits[] = {
    0xA0, 0x00, 0x00, 0x04, 0x92, 0x49, 0x24, 0x82, 0x00, 0x00, 0x10, 0x49,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na070_bits[] = {
    0x40, 0x02, 0x08, 0xA2, 0xE0, 0x00, 0x82, 0x28, 0x40,
};
static const uint8_t tvb_na071_bits[] = {
    0x11, 0x40,
};
static const uint8_t tvb_na072_bits[] = {
    0xA0, 0x90, 0x00, 0x00, 0x90, 0x00, 0x00, 0x10, 0x40, 0x04, 0x82, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na073_bits[] = {
    0xA0, 0x82, 0x08, 0x24, 0x10, 0x41, 0x00, 0x00, 0x00, 0x24, 0x92, 0x49,
    0x0A, 0x38, 0x00,
};
static const uint8_t tvb_na074_bits[] = {
    0xA4, 0x00, 0x41, 0x00, 0x92, 0x08, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na075_bits[] = {
    0x60, 0x00, 0x01, 0x04, 0x10, 0x49, 0x24, 0x82, 0x08, 0x2A, 0x00, 0x00,
    0x04, 0x10, 0x41, 0x24, 0x92, 0x08, 0x20, 0xA0,
};
static const uint8_t tvb_na076_bits[] = {
    0xA0, 0x92, 0x09, 0x04, 0x00, 0x40, 0x20, 0x10, 0x40, 0x04, 0x82, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na077_bits[] = {
    0x10, 0xA2, 0x62, 0x31, 0x98, 0x51, 0x31, 0x18, 0x00,
};
static const uint8_t tvb_na078_bits[] = {
    0x80, 0x45, 0x04, 0x01, 0x14, 0x10, 0x04, 0x50, 0x40,
};
static const uint8_t tvb_na079_bits[] = {
    0xA0, 0x82, 0x08, 0x24, 0x10, 0x41, 0x04, 0x90, 0x08, 0x20, 0x02, 0x41,
    0x0A, 0x38, 0x00,
};
static const uint8_t tvb_na080_bits[] = {
    0x81, 0x50, 0x40, 0xB8, 0x15, 0x04, 0x08,
};
static const uint8_t tvb_na081_bits[] = {
    0x18, 0x46, 0x18, 0x68, 0x47, 0x18, 0x46, 0x18, 0x68, 0x44,
};
static const uint8_t tvb_na082_bits[] = {
    0x0A, 0x12, 0x49, 0x2A, 0xB2, 0xA1, 0x24, 0x92, 0xA8,
};
static const uint8_t tvb_na083_bits[] = {
    0x10, 0x92, 0x49, 0x46, 0x33, 0x09, 0x24, 0x94, 0x60,
};
static const uint8_t tvb_na084_bits[] = {
    0x60, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00,
    0x04, 0x12, 0x48, 0x04, 0x12, 0x08, 0x2A, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x10, 0x49, 0x20, 0x10, 0x48,
    0x20, 0x80,
};
static const uint8_t tvb_na085_bits[] = {
    0x64, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0xA1, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x20, 0x10,
};
static const uint8_t tvb_na086_bits[] = {
    0x60, 0x82, 0x08, 0x20, 0x82, 0x41, 0x04, 0x92, 0x00, 0x20, 0x80, 0x40,
    0x00, 0x90, 0x40, 0x04, 0x00, 0x41, 0x2A, 0x02, 0x08, 0x20, 0x82, 0x09,
    0x04, 0x12, 0x48, 0x00, 0x82, 0x01, 0x00, 0x02, 0x41, 0x00, 0x10, 0x01,
    0x04, 0x80,
};
static const uint8_t tvb_na087_bits[] = {
    0xA0, 0x02, 0x40, 0x04, 0x90, 0x09, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na088_bits[] = {
    0x80, 0x00, 0x40, 0x04, 0x12, 0x08, 0x04, 0x92, 0x40, 0x00, 0x00, 0x09,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na089_bits[] = {
    0xA0, 0x02, 0x00, 0x04, 0x90, 0x49, 0x20, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na090_bits[] = {
    0x10, 0xAB, 0x11, 0x8C, 0xC2, 0xAC, 0x46, 0x00,
};
static const uint8_t tvb_na091_bits[] = {
    0xD5, 0x41, 0x51, 0x40, 0x14, 0x04, 0x2D, 0x54, 0x15, 0x14, 0x01, 0x40,
    0x41,
};
static const uint8_t tvb_na092_bits[] = {
    0xD1, 0x00, 0x11, 0x00, 0x04, 0x00, 0x11, 0x55, 0x6D, 0x10, 0x01, 0x10,
    0x00, 0x40, 0x01, 0x15, 0x55,
};
static const uint8_t tvb_na093_bits[] = {
    0x60, 0x90, 0x00, 0x20, 0x80, 0x00, 0x04, 0x02, 0x01, 0x00, 0x90, 0x48,
    0x2A, 0x02, 0x40, 0x00, 0x82, 0x00, 0x00, 0x10, 0x08, 0x04, 0x02, 0x41,
    0x20, 0x80,
};
static const uint8_t tvb_na094_bits[] = {
    0x10, 0x94, 0x62, 0x31, 0x98, 0x4A, 0x31, 0x18, 0x00,
};
static const uint8_t tvb_na095_bits[] = {
    0x80, 0x02, 0x00, 0x00, 0x02, 0x00, 0x04, 0x82, 0x00, 0x00, 0x10, 0x49,
    0x2A, 0x17, 0x08,
};
static const uint8_t tvb_na096_bits[] = {
    0x80, 0x80, 0x40, 0x04, 0x92, 0x49, 0x20, 0x92, 0x00, 0x04, 0x00, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na097_bits[] = {
    0x84, 0x80, 0x00, 0x24, 0x10, 0x41, 0x00, 0x80, 0x01, 0x24, 0x12, 0x48,
    0x0A, 0xBA, 0x40,
};
static const uint8_t tvb_na098_bits[] = {
    0xA0, 0x00, 0x00, 0x04, 0x92, 0x49, 0x24, 0x00, 0x41, 0x00, 0x92, 0x08,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na099_bits[] = {
    0x80, 0x00, 0x00, 0x04, 0x12, 0x48, 0x24, 0x00, 0x00, 0x00, 0x92, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na100_bits[] = {
    0x29, 0x59, 0x65, 0x55, 0xEA, 0x56, 0x59, 0x55, 0x70,
};
static const uint8_t tvb_na101_bits[] = {
    0xA0, 0x00, 0x09, 0x04, 0x92, 0x40, 0x20, 0x00, 0x00, 0x04, 0x92, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na102_bits[] = {
    0x64, 0x02, 0x08, 0x00, 0x02, 0x09, 0x04, 0x12, 0x49, 0x0A, 0x10, 0x08,
    0x20, 0x00, 0x08, 0x24, 0x10, 0x49, 0x24, 0x10,
};
static const uint8_t tvb_na103_bits[] = {
    0x80, 0x02, 0x00, 0x00, 0x02, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x97, 0x48,
};
static const uint8_t tvb_na104_bits[] = {
    0xA4, 0x00, 0x49, 0x00, 0x92, 0x00, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na105_bits[] = {
    0xA4, 0x80, 0x00, 0x20, 0x12, 0x49, 0x04, 0x92, 0x49, 0x20, 0x00, 0x00,
    0x0A, 0x38, 0x40,
};
static const uint8_t tvb_na106_bits[] = {
    0x80, 0x02, 0x00, 0x04, 0x90, 0x49, 0x24, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na107_bits[] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x97, 0x48,
};
static const uint8_t tvb_na108_bits[] = {
    0x80, 0x90, 0x40, 0x00, 0x90, 0x40, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x97, 0x48,
};
static const uint8_t tvb_na109_bits[] = {
    0xA0, 0x00, 0x08, 0x24, 0x92, 0x41, 0x00, 0x82, 0x00, 0x04, 0x10, 0x49,
    0x2E, 0x28, 0x00,
};
static const uint8_t tvb_na110_bits[] = {
    0xA4, 0x80, 0x00, 0x20, 0x12, 0x49, 0x00, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na111_bits[] = {
    0x84, 0x92, 0x49, 0x20, 0x00, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0xBA, 0x40,
};
static const uint8_t tvb_na112_bits[] = {
    0xA4, 0x00, 0x00, 0x00, 0x92, 0x49, 0x24, 0x00, 0x00, 0x00, 0x92, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_na113_bits[] = {
    0x60, 0x00, 0x00, 0x20, 0x02, 0x09, 0x04, 0x02, 0x01, 0x00, 0x90, 0x48,
    0x2A, 0x00, 0x00, 0x00, 0x80, 0x08, 0x24, 0x10, 0x08, 0x04, 0x02, 0x41,
    0x20, 0x80,
};
static const uint8_t tvb_na114_bits[] = {
    0x84, 0x10, 0x40, 0x08, 0x82, 0x08, 0x01, 0xD2, 0x08, 0x20, 0x04, 0x41,
    0x04, 0x00, 0x40,
};
static const uint8_t tvb_na115_bits[] = {
    0x60, 0x82, 0x00, 0x20, 0x80, 0x41, 0x04, 0x90, 0x41, 0x2A, 0x02, 0x08,
    0x00, 0x82, 0x01, 0x04, 0x12, 0x41, 0x04, 0x80,
};
static const uint8_t tvb_na116_bits[] = {
    0xA0, 0x00, 0x40, 0x04, 0x92, 0x09, 0x24, 0x00, 0x40, 0x00, 0x92, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na117_bits[] = {
    0x84, 0x00, 0x00, 0x08, 0x12, 0x40, 0x01, 0xD2, 0x00, 0x00, 0x04, 0x09,
    0x20, 0x00, 0x40,
};
static const uint8_t tvb_na118_bits[] = {
    0x84, 0x90, 0x49, 0x20, 0x02, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0xBA, 0x40,
};
static const uint8_t tvb_na119_bits[] = {
    0xA0, 0x10, 0x00, 0x04, 0x82, 0x49, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na120_bits[] = {
    0xA0, 0x12, 0x00, 0x04, 0x80, 0x49, 0x24, 0x92, 0x40, 0x00, 0x00, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na121_bits[] = {
    0xA0, 0x00, 0x40, 0x04, 0x92, 0x09, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na122_bits[] = {
    0x80, 0x00, 0x00, 0x00, 0x12, 0x49, 0x24, 0x90, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x12, 0x49, 0x24, 0x90, 0x0B,
};
static const uint8_t tvb_na123_bits[] = {
    0xA0, 0x02, 0x48, 0x04, 0x90, 0x01, 0x20, 0x12, 0x40, 0x04, 0x80, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na124_bits[] = {
    0x80, 0x00, 0x48, 0x04, 0x92, 0x01, 0x20, 0x00, 0x00, 0x04, 0x92, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_na125_bits[] = {
    0xA0, 0x02, 0x48, 0x04, 0x90, 0x01, 0x20, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na126_bits[] = {
    0xA4, 0x10, 0x00, 0x20, 0x82, 0x49, 0x00, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na127_bits[] = {
    0x1B, 0x59,
};
static const uint8_t tvb_na128_bits[] = {
    0x60, 0x02, 0x08, 0x00, 0x02, 0x49, 0x04, 0x12, 0x49, 0x0A, 0x00, 0x08,
    0x20, 0x00, 0x09, 0x24, 0x10, 0x49, 0x24, 0x00,
};
static const uint8_t tvb_na129_bits[] = {
    0xA4, 0x92, 0x49, 0x20, 0x00, 0x00, 0x00, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x38, 0x40,
};
static const uint8_t tvb_na130_bits[] = {
    0x64, 0x00, 0x08, 0x24, 0x82, 0x09, 0x24, 0x10, 0x01, 0x0A, 0x10, 0x00,
    0x20, 0x92, 0x08, 0x24, 0x90, 0x40, 0x04, 0x10,
};
static const uint8_t tvb_na131_bits[] = {
    0xA0, 0x10, 0x40, 0x04, 0x82, 0x09, 0x24, 0x82, 0x40, 0x00, 0x10, 0x09,
    0x2A, 0x38, 0x00,
};
static const uint8_t tvb_na132_bits[] = {
    0x22, 0x20, 0x00, 0x17, 0x22, 0x20, 0x00, 0x14,
};
static const uint8_t tvb_na133_bits[] = {
    0x09, 0x24, 0x49, 0x48, 0xB4, 0x92, 0x44, 0x94, 0x8C,
};
static const uint8_t tvb_na134_bits[] = {
    0x60, 0x90, 0x00, 0x24, 0x10, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x02, 0x40, 0x00, 0x90, 0x40, 0x00, 0x12, 0x48, 0x00, 0x00, 0x01,
    0x24, 0x80,
};
static const uint8_t tvb_na135_bits[] = {
    0x60, 0x12, 0x49, 0x00, 0x00, 0x09, 0x00, 0x00, 0x49, 0x24, 0x80, 0x00,
    0x00, 0x12, 0x49, 0x24, 0xA8, 0x01, 0x24, 0x90, 0x00, 0x00, 0x90, 0x00,
    0x04, 0x92, 0x48, 0x00, 0x00, 0x01, 0x24, 0x92, 0x48,
};
static const uint8_t tvb_na136_bits[] = {
    0x84, 0x82, 0x49, 0x00, 0x00, 0x00, 0x20, 0x00, 0x49, 0x24, 0x80, 0x00,
    0x00, 0x12, 0x49, 0x24, 0xAA, 0x48, 0x24, 0x90, 0x00, 0x00, 0x02, 0x00,
    0x04, 0x92, 0x48, 0x00, 0x00, 0x01, 0x24, 0x92, 0x4B,
};
static const uint8_t tvb_eu000_bits[] = {
    0xA4, 0x08, 0x00, 0x00, 0x00, 0x00, 0x64, 0x2C, 0x40, 0x80, 0x00, 0x00,
    0x00, 0x06, 0x41,
};
static const uint8_t tvb_eu001_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x35, 0x89, 0x24, 0x9A, 0xD6, 0x24, 0x92, 0x48,
};
static const uint8_t tvb_eu002_bits[] = {
    0x1A, 0x56, 0xA6, 0xD6, 0x95, 0xA9, 0x90,
};
static const uint8_t tvb_eu004_bits[] = {
    0x60, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x04, 0x12, 0x48, 0x04, 0x12, 0x48, 0x2A, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x10, 0x49, 0x20, 0x10, 0x49,
    0x20, 0x80,
};
static const uint8_t tvb_eu005_bits[] = {
    0x04, 0x92, 0x52, 0x28, 0x92, 0x8C, 0x44, 0x92, 0x89, 0x45, 0x24, 0x53,
    0x44, 0x92, 0x52, 0x28, 0x92, 0x8C, 0x44, 0x92, 0x89, 0x45, 0x24, 0x51,
};
static const uint8_t tvb_eu006_bits[] = {
    0x84, 0x90, 0x00, 0x04, 0x90, 0x00, 0x00, 0x80, 0x00, 0x04, 0x12, 0x49,
    0x2A, 0x12, 0x40, 0x00, 0x12, 0x40, 0x00, 0x02, 0x00, 0x00, 0x10, 0x49,
    0x24, 0xB0,
};
static const uint8_t tvb_eu007_bits[] = {
    0xD4, 0x00, 0x15, 0x10, 0x25, 0x00, 0x05, 0x44, 0x09, 0x40, 0x01, 0x51,
    0x01,
};
static const uint8_t tvb_eu008_bits[] = {
    0xA0, 0x00, 0x41, 0x04, 0x92, 0x08, 0x24, 0x90, 0x40, 0x00, 0x02, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu011_bits[] = {
    0x84, 0x00, 0x48, 0x04, 0x02, 0x01, 0x04, 0x80, 0x09, 0x00, 0x12, 0x40,
    0x2A, 0xBA, 0x40,
};
static const uint8_t tvb_eu012_bits[] = {
    0x05, 0x01, 0x51, 0x81, 0x40, 0x54, 0x40,
};
static const uint8_t tvb_eu013_bits[] = {
    0xD4, 0x55, 0x00, 0x00, 0x40, 0x15, 0x54, 0x00, 0x01, 0x55, 0x56, 0xD4,
    0x55, 0x00, 0x00, 0x40, 0x15, 0x54, 0x00, 0x01, 0x55, 0x55,
};
static const uint8_t tvb_eu015_bits[] = {
    0x60, 0x82, 0x08, 0x24, 0x10, 0x41, 0x00, 0x12, 0x40, 0x04, 0x80, 0x09,
    0x2A, 0x02, 0x08, 0x20, 0x90, 0x41, 0x04, 0x00, 0x49, 0x00, 0x12, 0x00,
    0x24, 0xA8, 0x08, 0x20, 0x82, 0x41, 0x04, 0x10, 0x01, 0x24, 0x00, 0x48,
    0x00, 0x92, 0xA0, 0x20, 0x82, 0x09, 0x04, 0x10, 0x40, 0x04, 0x90, 0x01,
    0x20, 0x02, 0x48,
};
static const uint8_t tvb_eu016_bits[] = {
    0x68, 0x08, 0x20, 0x00, 0xEA, 0x02, 0x08, 0x00, 0x10,
};
static const uint8_t tvb_eu017_bits[] = {
    0x1A, 0x9A, 0x9B, 0x9A, 0x9A, 0x99,
};
static const uint8_t tvb_eu018_bits[] = {
    0xA0, 0x02, 0x48, 0x04, 0x90, 0x01, 0x20, 0x12, 0x40, 0x04, 0x80, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu019_bits[] = {
    0x80, 0x80, 0x00, 0x08, 0x12, 0x40, 0x01, 0xC0, 0x40, 0x00, 0x04, 0x09,
    0x20, 0x00, 0x00,
};
static const uint8_t tvb_eu020_bits[] = {
    0x22, 0x20, 0x00, 0x01, 0xC8, 0x88, 0x00, 0x00, 0x40,
};
static const uint8_t tvb_eu021_bits[] = {
    0x84, 0x80, 0x00, 0x20, 0x82, 0x49, 0x00, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0xBA, 0x40,
};
static const uint8_t tvb_eu022_bits[] = {
    0xA4, 0x80, 0x41, 0x00, 0x12, 0x08, 0x24, 0x90, 0x40, 0x00, 0x02, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu024_bits[] = {
    0xA0, 0x02, 0x48, 0x04, 0x90, 0x01, 0x20, 0x00, 0x40, 0x04, 0x92, 0x09,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu025_bits[] = {
    0x47, 0x00, 0x23, 0x3C, 0x01, 0x59, 0xE0, 0x04,
};
static const uint8_t tvb_eu026_bits[] = {
    0x55, 0x40, 0x42, 0x55, 0x40, 0x41,
};
static const uint8_t tvb_eu027_bits[] = {
    0xA0, 0x82, 0x08, 0x24, 0x10, 0x41, 0x04, 0x10, 0x01, 0x20, 0x82, 0x48,
    0x0B, 0x3D, 0x00,
};
static const uint8_t tvb_eu028_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x34, 0x72, 0x24, 0x9A, 0xD1, 0xC8, 0x92, 0x48,
};
static const uint8_t tvb_eu029_bits[] = {
    0x47, 0x00, 0x00, 0x00, 0x00, 0x04, 0x64, 0x62, 0x00, 0xE0, 0x00, 0x2B,
    0x23, 0x10, 0x07, 0x00, 0x00, 0x80,
};
static const uint8_t tvb_eu030_bits[] = {
    0xA0, 0x10, 0x00, 0x04, 0x82, 0x49, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu031_bits[] = {
    0xD5, 0x50, 0x15, 0x11, 0x65, 0x54, 0x05, 0x44, 0x59, 0x55, 0x01, 0x51,
    0x15,
};
static const uint8_t tvb_eu032_bits[] = {
    0x1A, 0x56, 0xA5, 0xD6, 0x95, 0xA9, 0x40,
};
static const uint8_t tvb_eu033_bits[] = {
    0x2A, 0x5D, 0xA9, 0x60,
};
static const uint8_t tvb_eu034_bits[] = {
    0xA0, 0x02, 0x40, 0x04, 0x90, 0x09, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu036_bits[] = {
    0xA4, 0x00, 0x49, 0x00, 0x92, 0x00, 0x20, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu037_bits[] = {
    0x45, 0x50, 0x02, 0x45, 0x50, 0x01,
};
static const uint8_t tvb_eu038_bits[] = {
    0x05, 0x60, 0x54,
};
static const uint8_t tvb_eu039_bits[] = {
    0x6A, 0x82, 0x83, 0xAA, 0x82, 0x81,
};
static const uint8_t tvb_eu040_bits[] = {
    0x60, 0x90, 0x40, 0x20, 0x80, 0x40, 0x20, 0x90, 0x41, 0x2A, 0x02, 0x41,
    0x00, 0x82, 0x01, 0x00, 0x82, 0x41, 0x04, 0x80,
};
static const uint8_t tvb_eu041_bits[] = {
    0x0C, 0xB2, 0xCA, 0x49, 0x13, 0x0B, 0x2C, 0xB2, 0x92, 0x44, 0xB0,
};
static const uint8_t tvb_eu042_bits[] = {
    0x80, 0x00, 0x00, 0x24, 0x92, 0x09, 0x00, 0x82, 0x00, 0x04, 0x10, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_eu043_bits[] = {
    0x10,
};
static const uint8_t tvb_eu044_bits[] = {
    0xA0, 0x02, 0x01, 0x04, 0x90, 0x48, 0x20, 0x00, 0x00, 0x04, 0x92, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu045_bits[] = {
    0x16, 0xE5, 0x90,
};
static const uint8_t tvb_eu046_bits[] = {
    0x16, 0xAB, 0x56, 0xA9,
};
static const uint8_t tvb_eu047_bits[] = {
    0x41, 0x24, 0x12, 0x41, 0x00,
};
static const uint8_t tvb_eu048_bits[] = {
    0x80, 0x00, 0x00, 0x24, 0x82, 0x49, 0x04, 0x80, 0x40, 0x00, 0x12, 0x09,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_eu049_bits[] = {
    0x80, 0x92, 0x00, 0x00, 0x92, 0x00, 0x00, 0x10, 0x40, 0x04, 0x82, 0x09,
    0x2A, 0x97, 0x48,
};
static const uint8_t tvb_eu050_bits[] = {
    0x60, 0x00, 0x00, 0x00, 0x12, 0x49, 0x24, 0x92, 0x42, 0x80, 0x00, 0x00,
    0x00, 0x12, 0x49, 0x24, 0x92, 0x40,
};
static const uint8_t tvb_eu051_bits[] = {
    0x60, 0x82, 0x00, 0x20, 0x80, 0x41, 0x04, 0x90, 0x41, 0x2A, 0x02, 0x08,
    0x00, 0x82, 0x01, 0x04, 0x12, 0x41, 0x04, 0x80,
};
static const uint8_t tvb_eu052_bits[] = {
    0x1A, 0x9A, 0x9B, 0x9A, 0x9A, 0x99,
};
static const uint8_t tvb_eu053_bits[] = {
    0x26, 0xAB, 0x66, 0xAA,
};
static const uint8_t tvb_eu054_bits[] = {
    0x40, 0x1A, 0x23, 0x00, 0xD0, 0x80,
};
static const uint8_t tvb_eu055_bits[] = {
    0x80, 0x00, 0x00, 0x20, 0x92, 0x49, 0x00, 0x02, 0x40, 0x04, 0x90, 0x09,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_eu056_bits[] = {
    0x26,
};
static const uint8_t tvb_eu058_bits[] = {
    0x80, 0x00, 0x00, 0x24, 0x10, 0x49, 0x00, 0x82, 0x00, 0x04, 0x10, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_eu059_bits[] = {
    0x26,
};
static const uint8_t tvb_eu060_bits[] = {
    0x25, 0x59, 0x9A, 0x5A, 0xE9, 0x56, 0x66, 0x96, 0xA0,
};
static const uint8_t tvb_eu061_bits[] = {
    0x10, 0x92, 0x54, 0x24, 0xB3, 0x09, 0x25, 0x42, 0x48,
};
static const uint8_t tvb_eu062_bits[] = {
    0x25, 0x99, 0x9A, 0x5A, 0xE9, 0x66, 0x66, 0x96, 0xA0,
};
static const uint8_t tvb_eu063_bits[] = {
    0x80, 0x00, 0x00, 0x24, 0x90, 0x41, 0x00, 0x82, 0x00, 0x04, 0x10, 0x49,
    0x2A, 0xBA, 0x00,
};
static const uint8_t tvb_eu064_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x32, 0x51, 0xCB, 0xD6, 0x4A, 0x39, 0x72,
};
static const uint8_t tvb_eu065_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x32, 0x4A, 0x38, 0x9A, 0xC9, 0x28, 0xE2, 0x48,
};
static const uint8_t tvb_eu066_bits[] = {
    0x84, 0x82, 0x00, 0x04, 0x82, 0x00, 0x00, 0x82, 0x00, 0x04, 0x10, 0x49,
    0x2A, 0x87, 0x41,
};
static const uint8_t tvb_eu067_bits[] = {
    0x41, 0x24, 0x12,
};
static const uint8_t tvb_eu068_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x34, 0x49, 0x38, 0x9A, 0xD1, 0x24, 0xE2, 0x48,
};
static const uint8_t tvb_eu069_bits[] = {
    0x05, 0x54, 0x06, 0x05, 0x54, 0x04,
};
static const uint8_t tvb_eu070_bits[] = {
    0x14, 0x54, 0x06, 0x14, 0x54, 0x04,
};
static const uint8_t tvb_eu071_bits[] = {
    0x45, 0x44, 0x56, 0x45, 0x44, 0x55,
};
static const uint8_t tvb_eu072_bits[] = {
    0x55, 0x45, 0x46, 0x55, 0x45, 0x44,
};
static const uint8_t tvb_eu073_bits[] = {
    0x19, 0x57, 0x59, 0x55,
};
static const uint8_t tvb_eu074_bits[] = {
    0x04, 0x92, 0x49, 0x28, 0xC6, 0x49, 0x24, 0x92, 0x51, 0x80,
};
static const uint8_t tvb_eu075_bits[] = {
    0x05, 0x45, 0x46, 0x05, 0x45, 0x44,
};
static const uint8_t tvb_eu076_bits[] = {
    0x2A, 0x9A, 0x9B, 0xAA, 0x9A, 0x9A,
};
static const uint8_t tvb_eu077_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x32, 0x51, 0xC8, 0x9A, 0xC9, 0x47, 0x22, 0x48,
};
static const uint8_t tvb_eu078_bits[] = {
    0x90, 0x0D, 0x00,
};
static const uint8_t tvb_eu079_bits[] = {
    0x60, 0x00, 0x00, 0x24, 0x80, 0x09, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x00, 0x00, 0x00, 0x92, 0x00, 0x24, 0x12, 0x48, 0x00, 0x00, 0x01,
    0x24, 0x80,
};
static const uint8_t tvb_eu080_bits[] = {
    0x60, 0x00, 0x00, 0x20, 0x10, 0x09, 0x04, 0x02, 0x01, 0x00, 0x90, 0x48,
    0x2A, 0x00, 0x00, 0x00, 0x80, 0x40, 0x24, 0x10, 0x08, 0x04, 0x02, 0x41,
    0x20, 0x80,
};
static const uint8_t tvb_eu081_bits[] = {
    0x1A, 0x5A, 0x65, 0x67, 0x9A, 0x65, 0x9A, 0x9B, 0x9A, 0x5A, 0x65, 0x67,
    0x9A, 0x65, 0x9A, 0x9B, 0x9A, 0x5A, 0x65, 0x65,
};
static const uint8_t tvb_eu082_bits[] = {
    0x60, 0x82, 0x08, 0x24, 0x10, 0x41, 0x04, 0x82, 0x40, 0x00, 0x10, 0x09,
    0x2A, 0x02, 0x08, 0x20, 0x90, 0x41, 0x04, 0x12, 0x09, 0x00, 0x00, 0x40,
    0x24, 0x80,
};
static const uint8_t tvb_eu083_bits[] = {
    0x0E, 0x38, 0x21, 0x82, 0x26, 0x20, 0x82, 0x48, 0x23,
};
static const uint8_t tvb_eu084_bits[] = {
    0x6A, 0xA0, 0x03, 0xAA, 0xA0, 0x01,
};
static const uint8_t tvb_eu085_bits[] = {
    0x84, 0x82, 0x40, 0x08, 0x92, 0x48, 0x01, 0xC2, 0x41, 0x20, 0x04, 0x49,
    0x24, 0x00, 0x40,
};
static const uint8_t tvb_eu086_bits[] = {
    0x45, 0x86, 0x5B, 0x05, 0xC6, 0x5B, 0x05, 0xB0, 0x42,
};
static const uint8_t tvb_eu087_bits[] = {
    0x55, 0x50, 0x02, 0x55, 0x50, 0x01,
};
static const uint8_t tvb_eu088_bits[] = {
    0x45, 0x54, 0x42, 0x45, 0x54, 0x41,
};
static const uint8_t tvb_eu089_bits[] = {
    0x84, 0x10, 0x40, 0x08, 0x82, 0x08, 0x01, 0xC2, 0x08, 0x20, 0x04, 0x41,
    0x04, 0x00, 0x40,
};
static const uint8_t tvb_eu090_bits[] = {
    0x60, 0x00, 0x88, 0x00, 0x02, 0xE3, 0x00, 0x04, 0x40, 0x00, 0x16,
};
static const uint8_t tvb_eu091_bits[] = {
    0x80, 0x01, 0x00, 0x2E, 0x00, 0x04, 0x00, 0xA0,
};
static const uint8_t tvb_eu092_bits[] = {
    0x48, 0x80, 0x0E, 0x22, 0x00, 0x10,
};
static const uint8_t tvb_eu093_bits[] = {
    0x15, 0x9A, 0x94,
};
static const uint8_t tvb_eu094_bits[] = {
    0x60, 0x80, 0x88, 0x00, 0x00, 0xE3, 0x04, 0x04, 0x40, 0x00, 0x06,
};
static const uint8_t tvb_eu095_bits[] = {
    0x2A, 0xAB, 0x6A, 0xAA,
};
static const uint8_t tvb_eu096_bits[] = {
    0x04, 0x94, 0x4B, 0x24, 0x95, 0x35, 0x24, 0xA2, 0x59, 0x24, 0xA8, 0x40,
};
static const uint8_t tvb_eu097_bits[] = {
    0x19, 0xAB, 0x59, 0xA9,
};
static const uint8_t tvb_eu098_bits[] = {
    0x80, 0x01, 0x00, 0xB8, 0x55, 0x10, 0x08,
};
static const uint8_t tvb_eu099_bits[] = {
    0x46, 0x80, 0x00, 0x00, 0x00, 0x03, 0x44, 0x52, 0x00, 0x00, 0x0C, 0x22,
    0x22, 0x90, 0x00, 0x00, 0x60, 0x80,
};
static const uint8_t tvb_eu100_bits[] = {
    0x80, 0x04, 0x00, 0xB8, 0x55, 0x40, 0x08,
};
static const uint8_t tvb_eu101_bits[] = {
    0x55, 0x50, 0x06, 0x55, 0x50, 0x05,
};
static const uint8_t tvb_eu102_bits[] = {
    0x45, 0x54, 0x02, 0x45, 0x54, 0x01,
};
static const uint8_t tvb_eu103_bits[] = {
    0x29, 0x9A, 0x9B, 0xA9, 0x9A, 0x9A,
};
static const uint8_t tvb_eu104_bits[] = {
    0x44, 0x40, 0x02, 0x44, 0x40, 0x01,
};
static const uint8_t tvb_eu105_bits[] = {
    0x84, 0x10, 0x00, 0x20, 0x90, 0x01, 0x00, 0x80, 0x40, 0x04, 0x12, 0x09,
    0x2A, 0xBA, 0x40,
};
static const uint8_t tvb_eu106_bits[] = {
    0x0B, 0x12, 0x49, 0x24, 0x92, 0x49, 0x8D, 0x1C, 0x89, 0x27, 0xFC, 0xAB,
    0x47, 0x22, 0x49, 0xFF, 0x2A, 0xD1, 0xC8, 0x92, 0x7F, 0xC9, 0x00,
};
static const uint8_t tvb_eu107_bits[] = {
    0x62, 0x08, 0xA0, 0x8A, 0x19, 0x04, 0x08, 0x40, 0x83,
};
static const uint8_t tvb_eu108_bits[] = {
    0x45, 0x54, 0x16, 0x45, 0x54, 0x15,
};
static const uint8_t tvb_eu109_bits[] = {
    0x19, 0x95, 0x5E, 0x66, 0x55, 0x50,
};
static const uint8_t tvb_eu110_bits[] = {
    0x64, 0x10, 0x00, 0x04, 0x10, 0x00, 0x00, 0x80, 0x00, 0x04, 0x12, 0x49,
    0x2A, 0x10, 0x40, 0x00, 0x10, 0x40, 0x00, 0x02, 0x00, 0x00, 0x10, 0x49,
    0x24, 0x90,
};
static const uint8_t tvb_eu111_bits[] = {
    0x22, 0x80, 0x1A, 0x18, 0x01, 0x10, 0xC0, 0x02,
};
static const uint8_t tvb_eu112_bits[] = {
    0x80, 0x02, 0x00, 0x00, 0x02, 0x00, 0x04, 0x92, 0x00, 0x00, 0x00, 0x49,
    0x2A, 0x97, 0x48,
};
static const uint8_t tvb_eu113_bits[] = {
    0x46, 0x80, 0x23, 0x34, 0x00, 0x80,
};
static const uint8_t tvb_eu114_bits[] = {
    0x04, 0x92, 0x49, 0x26, 0x34, 0x71, 0x44, 0x9A, 0xD1, 0xC5, 0x12, 0x48,
};
static const uint8_t tvb_eu115_bits[] = {
    0x84, 0x92, 0x01, 0x24, 0x12, 0x00, 0x04, 0x80, 0x08, 0x09, 0x92, 0x48,
    0x04, 0x90, 0x48, 0x00, 0x12, 0x00, 0x20, 0x26, 0x49, 0x20, 0x12, 0x41,
    0x20, 0x00, 0x48, 0x00, 0x82,
};
static const uint8_t tvb_eu116_bits[] = {
    0x80, 0x01, 0x00, 0x2E, 0x00, 0x04, 0x00, 0x80,
};
static const uint8_t tvb_eu117_bits[] = {
    0x22, 0x00, 0x1A, 0x10, 0x00, 0x40,
};
static const uint8_t tvb_eu118_bits[] = {
    0x2A, 0x9A, 0x9B, 0xAA, 0x9A, 0x9A,
};
static const uint8_t tvb_eu119_bits[] = {
    0x44, 0x44, 0x02, 0x44, 0x44, 0x01,
};
static const uint8_t tvb_eu120_bits[] = {
    0x09, 0x24, 0x92, 0x49, 0x12, 0x4A, 0x24, 0x92, 0x49, 0x24, 0x92, 0x49,
    0x24, 0x94, 0x89, 0x69, 0x24, 0x92, 0x49, 0x22, 0x49, 0x44, 0x92, 0x49,
    0x24, 0x92, 0x49, 0x24, 0x92, 0x91, 0x30,
};
static const uint8_t tvb_eu121_bits[] = {
    0x64, 0x00, 0x09, 0x24, 0x00, 0x09, 0x24, 0x00, 0x09, 0x2A, 0x10, 0x00,
    0x24, 0x90, 0x00, 0x24, 0x90, 0x00, 0x24, 0x90,
};
static const uint8_t tvb_eu122_bits[] = {
    0x04, 0xA4, 0x92, 0x49, 0x22, 0x49, 0x48, 0x92, 0x49, 0x24, 0x92, 0x49,
    0x24, 0x94, 0x89, 0x68, 0x94, 0x92, 0x49, 0x24, 0x49, 0x29, 0x12, 0x49,
    0x24, 0x92, 0x49, 0x24, 0x92, 0x91, 0x30,
};
static const uint8_t tvb_eu123_bits[] = {
    0x6A, 0xA0, 0x0B, 0xAA, 0xA0, 0x09,
};
static const uint8_t tvb_eu124_bits[] = {
    0x80, 0x10, 0x40, 0x08, 0x92, 0x48, 0x01, 0xC0, 0x08, 0x20, 0x04, 0x49,
    0x24, 0x00, 0x00,
};
static const uint8_t tvb_eu125_bits[] = {
    0x84, 0x80, 0x00, 0x20, 0x82, 0x49, 0x00, 0x02, 0x00, 0x04, 0x90, 0x49,
    0x2A, 0x92, 0x00, 0x00, 0x82, 0x09, 0x24, 0x00, 0x08, 0x00, 0x12, 0x41,
    0x24, 0xB0,
};
static const uint8_t tvb_eu126_bits[] = {
    0xA0, 0x00, 0x00, 0x04, 0x92, 0x49, 0x20, 0x00, 0x00, 0x04, 0x92, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu127_bits[] = {
    0x44, 0x40, 0x56, 0x44, 0x40, 0x55,
};
static const uint8_t tvb_eu128_bits[] = {
    0x05, 0xC4, 0x59,
};
static const uint8_t tvb_eu129_bits[] = {
    0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x8C, 0x40, 0x03, 0xF1, 0xEB,
    0x23, 0x10, 0x00, 0xFC, 0x74,
};
static const uint8_t tvb_eu130_bits[] = {
    0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x8C, 0x40, 0x03, 0xE3, 0xEB,
    0x23, 0x10, 0x00, 0xF8, 0xF4,
};
static const uint8_t tvb_eu131_bits[] = {
    0x55, 0x55, 0x42, 0x55, 0x55, 0x41,
};
static const uint8_t tvb_eu132_bits[] = {
    0x05, 0x50, 0x06, 0x05, 0x50, 0x04,
};
static const uint8_t tvb_eu133_bits[] = {
    0x55, 0x54, 0x12, 0x55, 0x54, 0x11,
};
static const uint8_t tvb_eu134_bits[] = {
    0x40, 0x0A, 0x83, 0x80, 0x0A, 0x81,
};
static const uint8_t tvb_eu135_bits[] = {
    0x54, 0x45, 0x46, 0x54, 0x45, 0x44,
};
static const uint8_t tvb_eu136_bits[] = {
    0xA0, 0x00, 0x00, 0x04, 0x92, 0x49, 0x24, 0x00, 0x00, 0x00, 0x92, 0x49,
    0x2B, 0x3D, 0x00,
};
static const uint8_t tvb_eu137_bits[] = {
    0x14, 0x95, 0x4A, 0x35, 0x9A, 0x4A, 0xA5, 0x1B, 0x00,
};
static const uint8_t tvb_eu138_bits[] = {
    0x05, 0x60, 0x54,
};
static const uint8_t tvb_eu139_bits[] = {
    0x64, 0x92, 0x4A, 0x24, 0x92, 0xE3, 0x24, 0x92, 0x51, 0x24, 0x96, 0x00,
};

/* --- Code table --- */

static const IrTvbCode ir_tvb_codes[] = {
    { 38400, tvb_na000_pairs, tvb_na000_bits,  26, 2, IR_REGION_ALL},
    { 57143, tvb_na001_pairs, tvb_na001_bits,  52, 2, IR_REGION_NA},
    { 37037, tvb_na002_pairs, tvb_na002_bits, 100, 3, IR_REGION_NA},
    { 38610, tvb_na003_pairs, tvb_na003_bits,  64, 2, IR_REGION_NA},
    { 38610, tvb_na004_pairs, tvb_na004_bits,  38, 3, IR_REGION_ALL},
    { 35714, tvb_na005_pairs, tvb_na005_bits,  24, 3, IR_REGION_ALL},
    { 38462, tvb_na006_pairs, tvb_na006_bits,  68, 3, IR_REGION_NA},
    { 39216, tvb_na007_pairs, tvb_na007_bits,  34, 3, IR_REGION_NA},
    { 38462, tvb_na008_pairs, tvb_na008_bits,  68, 3, IR_REGION_NA},
    { 38462, tvb_na009_pairs, tvb_na009_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na010_pairs, tvb_na010_bits,  52, 2, IR_REGION_NA},
    { 38462, tvb_na011_pairs, tvb_na011_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na012_pairs, tvb_na012_bits,  52, 3, IR_REGION_NA},
    { 38462, tvb_na013_pairs, tvb_na013_bits,  48, 3, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na014_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na015_bits,  38, 3, IR_REGION_NA},
    { 34483, tvb_na016_pairs, tvb_na016_bits,  34, 2, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na017_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na018_pairs, tvb_na018_bits, 136, 3, IR_REGION_NA},
    { 38462, tvb_na019_pairs, tvb_na019_bits, 100, 3, IR_REGION_NA},
    { 38462, tvb_na020_pairs, tvb_na020_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na021_pairs, tvb_na021_bits,  38, 3, IR_REGION_ALL},
    { 38462, tvb_na022_pairs, tvb_na022_bits,  38, 3, IR_REGION_ALL},
    { 40000, tvb_na023_pairs, tvb_na023_bits,  44, 2, IR_REGION_NA},
    { 38462, tvb_na024_pairs, tvb_na024_bits,  26, 3, IR_REGION_NA},
    { 38462, tvb_na025_pairs, tvb_na025_bits,  52, 3, IR_REGION_NA},
    { 39216, tvb_na026_pairs, tvb_na026_bits,  34, 3, IR_REGION_NA},
    { 57143, tvb_na001_pairs, tvb_na027_bits,  52, 2, IR_REGION_NA},
    { 38610, tvb_na028_pairs, tvb_na028_bits,  36, 2, IR_REGION_NA},
    { 35842, tvb_na029_pairs, tvb_na029_bits,  22, 3, IR_REGION_NA},
    { 38462, tvb_na009_pairs, tvb_na030_bits,  38, 3, IR_REGION_NA},
    { 35842, tvb_na031_pairs, tvb_na031_bits,  24, 3, IR_REGION_NA},
    { 38462, tvb_na009_pairs, tvb_na032_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na033_pairs, tvb_na033_bits, 100, 3, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na034_bits,  38, 3, IR_REGION_NA},
    { 41667, tvb_na035_pairs, tvb_na035_bits,  22, 2, IR_REGION_NA},
    { 37037, tvb_na036_pairs, tvb_na036_bits,  11, 2, IR_REGION_NA},
    { 41667, tvb_na037_pairs, tvb_na037_bits,  11, 2, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na038_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na039_pairs, tvb_na039_bits,   4, 2, IR_REGION_NA},
    { 40000, tvb_na040_pairs, tvb_na040_bits,   8, 2, IR_REGION_NA},
    { 76923, tvb_na041_pairs, tvb_na041_bits,  26, 2, IR_REGION_NA},
    { 40000, tvb_na042_pairs, tvb_na042_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na043_pairs, tvb_na043_bits,  24, 2, IR_REGION_NA},
    { 40000, tvb_na044_pairs, tvb_na044_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na045_pairs, tvb_na045_bits,  40, 3, IR_REGION_NA},
    { 29412, tvb_na046_pairs, tvb_na046_bits,  23, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na047_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na044_pairs, tvb_na048_bits,  38, 3, IR_REGION_NA},
    { 45455, tvb_na049_pairs, tvb_na049_bits,  11, 2, IR_REGION_NA},
    { 55556, tvb_na050_pairs, tvb_na050_bits,  48, 2, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na051_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na052_bits,  38, 3, IR_REGION_NA},
    { 55556, tvb_na053_pairs, tvb_na053_bits,  30, 2, IR_REGION_NA},
    { 55556, tvb_na053_pairs, tvb_na054_bits,  28, 2, IR_REGION_NA},
    {     0, tvb_na055_pairs, tvb_na055_bits,  27, 2, IR_REGION_NA},
    { 37175, tvb_na056_pairs, tvb_na056_bits,   8, 2, IR_REGION_NA},
    { 40000, tvb_na057_pairs, tvb_na057_bits,  14, 2, IR_REGION_NA},
    { 33333, tvb_na058_pairs, tvb_na058_bits,  18, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na059_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na060_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na061_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na062_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na063_bits,  38, 3, IR_REGION_NA},
    { 57143, tvb_na001_pairs, tvb_na064_bits,  52, 2, IR_REGION_NA},
    { 59172, tvb_na065_pairs, tvb_na065_bits,  78, 3, IR_REGION_NA},
    { 38462, tvb_na066_pairs, tvb_na066_bits,  33, 2, IR_REGION_NA},
    { 40000, tvb_na044_pairs, tvb_na067_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na068_pairs, tvb_na068_bits,  24, 2, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na069_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na070_pairs, tvb_na070_bits,  33, 2, IR_REGION_NA},
    { 55556, tvb_na071_pairs, tvb_na071_bits,   8, 2, IR_REGION_NA},
    { 40000, tvb_na042_pairs, tvb_na072_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na073_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na074_bits,  38, 3, IR_REGION_NA},
    { 41667, tvb_na075_pairs, tvb_na075_bits,  52, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na076_bits,  38, 3, IR_REGION_NA},
    { 35714, tvb_na031_pairs, tvb_na077_bits,  22, 3, IR_REGION_NA},
    { 38462, tvb_na078_pairs, tvb_na078_bits,  34, 2, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na079_bits,  38, 3, IR_REGION_NA},
    {     0, tvb_na055_pairs, tvb_na080_bits,  27, 2, IR_REGION_NA},
    { 40000, tvb_na081_pairs, tvb_na081_bits,  40, 2, IR_REGION_NA},
    { 35714, tvb_na082_pairs, tvb_na082_bits,  24, 3, IR_REGION_NA},
    { 35714, tvb_na031_pairs, tvb_na083_bits,  24, 3, IR_REGION_NA},
    { 37037, tvb_na084_pairs, tvb_na084_bits, 100, 3, IR_REGION_NA},
    { 38462, tvb_na085_pairs, tvb_na085_bits,  44, 3, IR_REGION_NA},
    { 37175, tvb_na086_pairs, tvb_na086_bits, 100, 3, IR_REGION_NA},
    { 38462, tvb_na087_pairs, tvb_na087_bits,  38, 3, IR_REGION_NA},
    { 38610, tvb_na009_pairs, tvb_na088_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na089_bits,  38, 3, IR_REGION_NA},
    { 35714, tvb_na090_pairs, tvb_na090_bits,  20, 3, IR_REGION_NA},
    { 58824, tvb_na091_pairs, tvb_na091_bits,  52, 2, IR_REGION_NA},
    { 38462, tvb_na092_pairs, tvb_na092_bits,  68, 2, IR_REGION_NA},
    { 38462, tvb_na093_pairs, tvb_na093_bits,  68, 3, IR_REGION_NA},
    { 35714, tvb_na005_pairs, tvb_na094_bits,  22, 3, IR_REGION_NA},
    { 38462, tvb_na095_pairs, tvb_na095_bits,  40, 3, IR_REGION_NA},
    { 38462, tvb_na009_pairs, tvb_na096_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na009_pairs, tvb_na097_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na098_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na009_pairs, tvb_na099_bits,  38, 3, IR_REGION_NA},
    { 35842, tvb_na100_pairs, tvb_na100_bits,  34, 2, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na101_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na102_pairs, tvb_na102_bits,  52, 3, IR_REGION_NA},
    { 38462, tvb_na045_pairs, tvb_na103_bits,  40, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na104_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na105_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na044_pairs, tvb_na106_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na045_pairs, tvb_na107_bits,  40, 3, IR_REGION_NA},
    { 38462, tvb_na045_pairs, tvb_na108_bits,  40, 3, IR_REGION_NA},
    { 40000, tvb_na109_pairs, tvb_na109_bits,  38, 3, IR_REGION_NA},
    { 40161, tvb_na017_pairs, tvb_na110_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na044_pairs, tvb_na111_bits,  38, 3, IR_REGION_NA},
    { 38462, tvb_na004_pairs, tvb_na112_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na113_pairs, tvb_na113_bits,  68, 3, IR_REGION_NA},
    { 40000, tvb_na114_pairs, tvb_na114_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na115_pairs, tvb_na115_bits,  52, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na116_bits,  38, 3, IR_REGION_NA},
    { 41667, tvb_na117_pairs, tvb_na117_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na044_pairs, tvb_na118_bits,  38, 3, IR_REGION_NA},
    { 55556, tvb_na119_pairs, tvb_na119_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na120_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na121_bits,  38, 3, IR_REGION_NA},
    { 52632, tvb_na122_pairs, tvb_na122_bits,  48, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na123_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na124_pairs, tvb_na124_bits,  38, 3, IR_REGION_NA},
    { 55556, tvb_na119_pairs, tvb_na125_bits,  38, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na126_bits,  38, 3, IR_REGION_NA},
    { 25641, tvb_na127_pairs, tvb_na127_bits,   8, 2, IR_REGION_NA},
    { 40000, tvb_na102_pairs, tvb_na128_bits,  52, 3, IR_REGION_NA},
    { 40000, tvb_na017_pairs, tvb_na129_bits,  38, 3, IR_REGION_NA},
    { 37037, tvb_na130_pairs, tvb_na130_bits,  52, 3, IR_REGION_NA},
    { 40000, tvb_na042_pairs, tvb_na131_bits,  38, 3, IR_REGION_NA},
    { 83333, tvb_na132_pairs, tvb_na132_bits,  32, 2, IR_REGION_NA},
    { 41667, tvb_na133_pairs, tvb_na133_bits,  24, 3, IR_REGION_NA},
    { 40000, tvb_na113_pairs, tvb_na134_bits,  68, 3, IR_REGION_NA},
    { 38462, tvb_na135_pairs, tvb_na135_bits,  88, 3, IR_REGION_NA},
    { 38610, tvb_na136_pairs, tvb_na136_bits,  88, 3, IR_REGION_NA},
    { 35714, tvb_eu000_pairs, tvb_eu000_bits,  40, 3, IR_REGION_EU},
    { 30303, tvb_eu001_pairs, tvb_eu001_bits,  31, 3, IR_REGION_EU},
    { 33333, tvb_eu002_pairs, tvb_eu002_bits,  26, 2, IR_REGION_EU},
    { 37037, tvb_eu004_pairs, tvb_eu004_bits, 100, 3, IR_REGION_EU},
    { 38610, tvb_eu005_pairs, tvb_eu005_bits,  64, 3, IR_REGION_EU},
    { 38462, tvb_eu006_pairs, tvb_eu006_bits,  68, 3, IR_REGION_EU},
    { 38462, tvb_eu007_pairs, tvb_eu007_bits,  52, 2, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu008_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu011_bits,  38, 3, IR_REGION_EU},
    { 33445, tvb_eu012_pairs, tvb_eu012_bits,  26, 2, IR_REGION_EU},
    { 38462, tvb_eu013_pairs, tvb_eu013_bits,  88, 2, IR_REGION_EU},
    { 38462, tvb_eu015_pairs, tvb_eu015_bits, 136, 3, IR_REGION_EU},
    { 33333, tvb_eu016_pairs, tvb_eu016_bits,  34, 2, IR_REGION_EU},
    { 33333, tvb_eu017_pairs, tvb_eu017_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu018_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu019_pairs, tvb_eu019_bits,  38, 3, IR_REGION_EU},
    { 35714, tvb_eu020_pairs, tvb_eu020_bits,  34, 2, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu021_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu022_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu024_bits,  38, 3, IR_REGION_EU},
    { 31250, tvb_eu025_pairs, tvb_eu025_bits,  21, 3, IR_REGION_EU},
    { 38462, tvb_eu026_pairs, tvb_eu026_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu027_bits,  38, 3, IR_REGION_EU},
    { 30303, tvb_eu028_pairs, tvb_eu028_bits,  31, 3, IR_REGION_EU},
    { 34483, tvb_eu029_pairs, tvb_eu029_bits,  46, 3, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu030_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu031_pairs, tvb_eu031_bits,  52, 2, IR_REGION_EU},
    { 33333, tvb_eu032_pairs, tvb_eu032_bits,  26, 2, IR_REGION_EU},
    { 38462, tvb_eu033_pairs, tvb_eu033_bits,  14, 2, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu034_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu036_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu037_pairs, tvb_eu037_bits,  24, 2, IR_REGION_EU},
    {     0, tvb_eu038_pairs, tvb_eu038_bits,  11, 2, IR_REGION_EU},
    { 40161, tvb_eu039_pairs, tvb_eu039_bits,  24, 2, IR_REGION_EU},
    { 35714, tvb_eu040_pairs, tvb_eu040_bits,  52, 3, IR_REGION_EU},
    { 33333, tvb_eu041_pairs, tvb_eu041_bits,  28, 3, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu042_bits,  38, 3, IR_REGION_EU},
    { 41667, tvb_eu043_pairs, tvb_eu043_bits,   2, 2, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu044_bits,  38, 3, IR_REGION_EU},
    { 41667, tvb_eu045_pairs, tvb_eu045_bits,  10, 2, IR_REGION_EU},
    { 34602, tvb_eu046_pairs, tvb_eu046_bits,  16, 2, IR_REGION_EU},
    {     0, tvb_eu047_pairs, tvb_eu047_bits,  17, 2, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu048_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu049_pairs, tvb_eu049_bits,  40, 3, IR_REGION_EU},
    { 33333, tvb_eu050_pairs, tvb_eu050_bits,  48, 3, IR_REGION_EU},
    { 38462, tvb_eu051_pairs, tvb_eu051_bits,  52, 3, IR_REGION_EU},
    { 31250, tvb_eu052_pairs, tvb_eu052_bits,  24, 2, IR_REGION_EU},
    { 34483, tvb_eu046_pairs, tvb_eu053_bits,  16, 2, IR_REGION_EU},
    { 31250, tvb_eu054_pairs, tvb_eu054_bits,  14, 3, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu055_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu056_pairs, tvb_eu056_bits,   4, 2, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu058_bits,  38, 3, IR_REGION_EU},
    { 41667, tvb_eu059_pairs, tvb_eu059_bits,   4, 2, IR_REGION_EU},
    { 38462, tvb_eu060_pairs, tvb_eu060_bits,  34, 2, IR_REGION_EU},
    { 35714, tvb_na005_pairs, tvb_eu061_bits,  24, 3, IR_REGION_EU},
    { 38462, tvb_eu060_pairs, tvb_eu062_bits,  34, 2, IR_REGION_EU},
    { 38462, tvb_na009_pairs, tvb_eu063_bits,  38, 3, IR_REGION_EU},
    { 30395, tvb_eu064_pairs, tvb_eu064_bits,  29, 3, IR_REGION_EU},
    { 30303, tvb_eu065_pairs, tvb_eu065_bits,  31, 3, IR_REGION_EU},
    { 38462, tvb_eu049_pairs, tvb_eu066_bits,  40, 3, IR_REGION_EU},
    { 38462, tvb_eu067_pairs, tvb_eu067_bits,  12, 2, IR_REGION_EU},
    { 38610, tvb_eu068_pairs, tvb_eu068_bits,  31, 3, IR_REGION_EU},
    {     0, tvb_eu069_pairs, tvb_eu069_bits,  23, 2, IR_REGION_EU},
    {     0, tvb_eu069_pairs, tvb_eu070_bits,  23, 2, IR_REGION_EU},
    { 38462, tvb_eu071_pairs, tvb_eu071_bits,  24, 2, IR_REGION_EU},
    {     0, tvb_eu072_pairs, tvb_eu072_bits,  23, 2, IR_REGION_EU},
    { 34483, tvb_eu046_pairs, tvb_eu073_bits,  16, 2, IR_REGION_EU},
    { 35714, tvb_na031_pairs, tvb_eu074_bits,  26, 3, IR_REGION_EU},
    {     0, tvb_eu075_pairs, tvb_eu075_bits,  23, 2, IR_REGION_EU},
    { 38462, tvb_eu076_pairs, tvb_eu076_bits,  24, 2, IR_REGION_EU},
    { 30303, tvb_eu028_pairs, tvb_eu077_bits,  31, 3, IR_REGION_EU},
    {     0, tvb_eu078_pairs, tvb_eu078_bits,  12, 2, IR_REGION_EU},
    { 38462, tvb_eu079_pairs, tvb_eu079_bits,  68, 3, IR_REGION_EU},
    { 38462, tvb_eu080_pairs, tvb_eu080_bits,  68, 3, IR_REGION_EU},
    { 38462, tvb_eu081_pairs, tvb_eu081_bits,  80, 2, IR_REGION_EU},
    { 40000, tvb_eu082_pairs, tvb_eu082_bits,  68, 3, IR_REGION_EU},
    { 33333, tvb_eu083_pairs, tvb_eu083_bits,  24, 3, IR_REGION_EU},
    { 38462, tvb_eu084_pairs, tvb_eu084_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu085_pairs, tvb_eu085_bits,  38, 3, IR_REGION_EU},
    { 33333, tvb_eu086_pairs, tvb_eu086_bits,  24, 3, IR_REGION_EU},
    { 38462, tvb_eu087_pairs, tvb_eu087_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu088_pairs, tvb_eu088_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_na021_pairs, tvb_eu089_bits,  38, 3, IR_REGION_EU},
    {     0, tvb_eu090_pairs, tvb_eu090_bits,  29, 3, IR_REGION_EU},
    { 38462, tvb_eu091_pairs, tvb_eu091_bits,  30, 2, IR_REGION_EU},
    { 40000, tvb_eu092_pairs, tvb_eu092_bits,  22, 2, IR_REGION_EU},
    { 35714, tvb_eu093_pairs, tvb_eu093_bits,  11, 2, IR_REGION_EU},
    {     0, tvb_eu094_pairs, tvb_eu094_bits,  29, 3, IR_REGION_EU},
    { 34483, tvb_eu046_pairs, tvb_eu095_bits,  16, 2, IR_REGION_EU},
    { 38462, tvb_eu096_pairs, tvb_eu096_bits,  30, 3, IR_REGION_EU},
    { 34483, tvb_eu046_pairs, tvb_eu097_bits,  16, 2, IR_REGION_EU},
    {     0, tvb_eu098_pairs, tvb_eu098_bits,  27, 2, IR_REGION_EU},
    { 35714, tvb_eu099_pairs, tvb_eu099_bits,  46, 3, IR_REGION_EU},
    {     0, tvb_eu098_pairs, tvb_eu100_bits,  27, 2, IR_REGION_EU},
    { 38462, tvb_eu101_pairs, tvb_eu101_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu087_pairs, tvb_eu102_bits,  24, 2, IR_REGION_EU},
    { 34483, tvb_eu103_pairs, tvb_eu103_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu104_pairs, tvb_eu104_bits,  24, 2, IR_REGION_EU},
    { 38610, tvb_na009_pairs, tvb_eu105_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu106_pairs, tvb_eu106_bits,  59, 3, IR_REGION_EU},
    { 33333, tvb_eu107_pairs, tvb_eu107_bits,  24, 3, IR_REGION_EU},
    { 38462, tvb_eu108_pairs, tvb_eu108_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu109_pairs, tvb_eu109_bits,  22, 2, IR_REGION_EU},
    { 38462, tvb_eu110_pairs, tvb_eu110_bits,  68, 3, IR_REGION_EU},
    { 31250, tvb_eu111_pairs, tvb_eu111_bits,  21, 3, IR_REGION_EU},
    { 38462, tvb_eu112_pairs, tvb_eu112_bits,  40, 3, IR_REGION_EU},
    { 31250, tvb_eu054_pairs, tvb_eu113_bits,  14, 3, IR_REGION_EU},
    { 30303, tvb_eu028_pairs, tvb_eu114_bits,  31, 3, IR_REGION_EU},
    { 58824, tvb_eu115_pairs, tvb_eu115_bits,  77, 3, IR_REGION_EU},
    {     0, tvb_eu116_pairs, tvb_eu116_bits,  29, 2, IR_REGION_EU},
    { 31250, tvb_eu117_pairs, tvb_eu117_bits,  14, 3, IR_REGION_EU},
    { 34483, tvb_eu118_pairs, tvb_eu118_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu119_pairs, tvb_eu119_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu120_pairs, tvb_eu120_bits,  82, 3, IR_REGION_EU},
    { 38462, tvb_eu051_pairs, tvb_eu121_bits,  52, 3, IR_REGION_EU},
    { 38462, tvb_eu120_pairs, tvb_eu122_bits,  82, 3, IR_REGION_EU},
    { 40000, tvb_eu123_pairs, tvb_eu123_bits,  24, 2, IR_REGION_EU},
    { 38462, tvb_eu124_pairs, tvb_eu124_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu125_pairs, tvb_eu125_bits,  68, 3, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu126_bits,  38, 3, IR_REGION_EU},
    { 38462, tvb_eu087_pairs, tvb_eu127_bits,  24, 2, IR_REGION_EU},
    { 41667, tvb_eu128_pairs, tvb_eu128_bits,   8, 3, IR_REGION_EU},
    { 38462, tvb_eu129_pairs, tvb_eu129_bits,  45, 3, IR_REGION_EU},
    { 38462, tvb_eu129_pairs, tvb_eu130_bits,  45, 3, IR_REGION_EU},
    { 38462, tvb_eu131_pairs, tvb_eu131_bits,  24, 2, IR_REGION_EU},
    {     0, tvb_eu069_pairs, tvb_eu132_bits,  23, 2, IR_REGION_EU},
    { 38462, tvb_eu071_pairs, tvb_eu133_bits,  24, 2, IR_REGION_EU},
    { 40000, tvb_eu134_pairs, tvb_eu134_bits,  24, 2, IR_REGION_EU},
    {     0, tvb_eu135_pairs, tvb_eu135_bits,  23, 2, IR_REGION_EU},
    { 38462, tvb_na004_pairs, tvb_eu136_bits,  38, 3, IR_REGION_EU},
    { 35714, tvb_eu137_pairs, tvb_eu137_bits,  22, 3, IR_REGION_EU},
    {     0, tvb_eu138_pairs, tvb_eu138_bits,  11, 2, IR_REGION_EU},
    {     0, tvb_eu139_pairs, tvb_eu139_bits,  30, 3, IR_REGION_EU},
};

/* Upstream names, in the same order as ir_tvb_codes, for the progress
   display and the event log. */
static const char *const ir_tvb_names[] = {
    "na000", "na001", "na002", "na003", "na004", "na005", "na006", "na007",
    "na008", "na009", "na010", "na011", "na012", "na013", "na014", "na015",
    "na016", "na017", "na018", "na019", "na020", "na021", "na022", "na023",
    "na024", "na025", "na026", "na027", "na028", "na029", "na030", "na031",
    "na032", "na033", "na034", "na035", "na036", "na037", "na038", "na039",
    "na040", "na041", "na042", "na043", "na044", "na045", "na046", "na047",
    "na048", "na049", "na050", "na051", "na052", "na053", "na054", "na055",
    "na056", "na057", "na058", "na059", "na060", "na061", "na062", "na063",
    "na064", "na065", "na066", "na067", "na068", "na069", "na070", "na071",
    "na072", "na073", "na074", "na075", "na076", "na077", "na078", "na079",
    "na080", "na081", "na082", "na083", "na084", "na085", "na086", "na087",
    "na088", "na089", "na090", "na091", "na092", "na093", "na094", "na095",
    "na096", "na097", "na098", "na099", "na100", "na101", "na102", "na103",
    "na104", "na105", "na106", "na107", "na108", "na109", "na110", "na111",
    "na112", "na113", "na114", "na115", "na116", "na117", "na118", "na119",
    "na120", "na121", "na122", "na123", "na124", "na125", "na126", "na127",
    "na128", "na129", "na130", "na131", "na132", "na133", "na134", "na135",
    "na136", "eu000", "eu001", "eu002", "eu004", "eu005", "eu006", "eu007",
    "eu008", "eu011", "eu012", "eu013", "eu015", "eu016", "eu017", "eu018",
    "eu019", "eu020", "eu021", "eu022", "eu024", "eu025", "eu026", "eu027",
    "eu028", "eu029", "eu030", "eu031", "eu032", "eu033", "eu034", "eu036",
    "eu037", "eu038", "eu039", "eu040", "eu041", "eu042", "eu043", "eu044",
    "eu045", "eu046", "eu047", "eu048", "eu049", "eu050", "eu051", "eu052",
    "eu053", "eu054", "eu055", "eu056", "eu058", "eu059", "eu060", "eu061",
    "eu062", "eu063", "eu064", "eu065", "eu066", "eu067", "eu068", "eu069",
    "eu070", "eu071", "eu072", "eu073", "eu074", "eu075", "eu076", "eu077",
    "eu078", "eu079", "eu080", "eu081", "eu082", "eu083", "eu084", "eu085",
    "eu086", "eu087", "eu088", "eu089", "eu090", "eu091", "eu092", "eu093",
    "eu094", "eu095", "eu096", "eu097", "eu098", "eu099", "eu100", "eu101",
    "eu102", "eu103", "eu104", "eu105", "eu106", "eu107", "eu108", "eu109",
    "eu110", "eu111", "eu112", "eu113", "eu114", "eu115", "eu116", "eu117",
    "eu118", "eu119", "eu120", "eu121", "eu122", "eu123", "eu124", "eu125",
    "eu126", "eu127", "eu128", "eu129", "eu130", "eu131", "eu132", "eu133",
    "eu134", "eu135", "eu136", "eu137", "eu138", "eu139",
};

#define IR_TVB_COUNT 270
