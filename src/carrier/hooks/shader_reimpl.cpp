// Reimplemented pixel shaders: the replacement list and the substitution —
// see shader_reimpl.hpp. One line per reimplemented shader.

#include "hooks/shader_reimpl.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "log.hpp"
#include "reimpl/render/shadow_surfaces.hpp"
#include "shaders/PgBillboardTreeFP.h"
#include "shaders/PgBillboardTreeFP_modern.h"
#include "shaders/PgDecal2FP.h"
#include "shaders/PgDecal2FP_modern.h"
#include "shaders/PgDecal2FP_pl.h"
#include "shaders/PgDecal2FP_pl_modern.h"
#include "shaders/PgDecal2FP_pl_li.h"
#include "shaders/PgDecal2FP_pl_li_modern.h"
#include "shaders/PgDecal2FP_pl_sl.h"
#include "shaders/PgDecal2FP_pl_sl_modern.h"
#include "shaders/PgDecal2FP_pl_sl_li.h"
#include "shaders/PgDecal2FP_pl_sl_li_modern.h"
#include "shaders/PgDecal2FP_sl.h"
#include "shaders/PgDecal2FP_sl_modern.h"
#include "shaders/PgDecal2FP_sl_li.h"
#include "shaders/PgDecal2FP_sl_li_modern.h"
#include "shaders/PgDiffRefractFP.h"
#include "shaders/PgDiffRefractNormFP.h"
#include "shaders/PgDiffSpecMetalNormFP.h"
#include "shaders/PgDiffSpecMetalNormFP_modern.h"
#include "shaders/PgDiffSpecMetalNormFP_pl.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_li.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalNormFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormFP_sl.h"
#include "shaders/PgDiffSpecMetalNormFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormFP_sl_li.h"
#include "shaders/PgDiffSpecMetalNormFP_sl_li_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP.h"
#include "shaders/PgDiffSpecNormAmbOccFP_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecNormAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP_sl.h"
#include "shaders/PgDiffSpecNormAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecNormAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecNormAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecNormFP.h"
#include "shaders/PgDiffSpecNormFP_modern.h"
#include "shaders/PgDiffSpecNormFP_pl.h"
#include "shaders/PgDiffSpecNormFP_pl_modern.h"
#include "shaders/PgDiffSpecNormFP_pl_li.h"
#include "shaders/PgDiffSpecNormFP_pl_li_modern.h"
#include "shaders/PgDiffSpecNormFP_pl_sl.h"
#include "shaders/PgDiffSpecNormFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecNormFP_pl_sl_li.h"
#include "shaders/PgDiffSpecNormFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecNormFP_sl.h"
#include "shaders/PgDiffSpecNormFP_sl_modern.h"
#include "shaders/PgDiffSpecNormFP_sl_li.h"
#include "shaders/PgDiffSpecNormFP_sl_li_modern.h"
#include "shaders/PgDiffAmbOccFP.h"
#include "shaders/PgDiffAmbOccFP_modern.h"
#include "shaders/PgDiffAmbOccFP_pl.h"
#include "shaders/PgDiffAmbOccFP_pl_modern.h"
#include "shaders/PgDiffAmbOccFP_pl_li.h"
#include "shaders/PgDiffAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffAmbOccFP_pl_sl.h"
#include "shaders/PgDiffAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffAmbOccFP_sl.h"
#include "shaders/PgDiffAmbOccFP_sl_modern.h"
#include "shaders/PgDiffAmbOccFP_sl_li.h"
#include "shaders/PgDiffAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffAmbOccRimFP.h"
#include "shaders/PgDiffAmbOccRimFP_modern.h"
#include "shaders/PgDiffAmbOccRimFP_pl.h"
#include "shaders/PgDiffAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffAmbOccRimFP_sl.h"
#include "shaders/PgDiffAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffEmisAmbOccFP.h"
#include "shaders/PgDiffEmisAmbOccFP_modern.h"
#include "shaders/PgDiffEmisAmbOccFP_pl.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_modern.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_li.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_sl.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffEmisAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffEmisAmbOccFP_sl.h"
#include "shaders/PgDiffEmisAmbOccFP_sl_modern.h"
#include "shaders/PgDiffEmisAmbOccFP_sl_li.h"
#include "shaders/PgDiffEmisAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP.h"
#include "shaders/PgDiffEmisAmbOccRimFP_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffEmisAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP_sl.h"
#include "shaders/PgDiffEmisAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffEmisAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffEmisAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffEmisFP.h"
#include "shaders/PgDiffEmisFP_modern.h"
#include "shaders/PgDiffEmisFP_pl.h"
#include "shaders/PgDiffEmisFP_pl_modern.h"
#include "shaders/PgDiffEmisFP_pl_li.h"
#include "shaders/PgDiffEmisFP_pl_li_modern.h"
#include "shaders/PgDiffEmisFP_pl_sl.h"
#include "shaders/PgDiffEmisFP_pl_sl_modern.h"
#include "shaders/PgDiffEmisFP_pl_sl_li.h"
#include "shaders/PgDiffEmisFP_pl_sl_li_modern.h"
#include "shaders/PgDiffEmisFP_sl.h"
#include "shaders/PgDiffEmisFP_sl_modern.h"
#include "shaders/PgDiffEmisFP_sl_li.h"
#include "shaders/PgDiffEmisFP_sl_li_modern.h"
#include "shaders/PgDiffEmisRimFP.h"
#include "shaders/PgDiffEmisRimFP_modern.h"
#include "shaders/PgDiffEmisRimFP_pl.h"
#include "shaders/PgDiffEmisRimFP_pl_modern.h"
#include "shaders/PgDiffEmisRimFP_pl_li.h"
#include "shaders/PgDiffEmisRimFP_pl_li_modern.h"
#include "shaders/PgDiffEmisRimFP_pl_sl.h"
#include "shaders/PgDiffEmisRimFP_pl_sl_modern.h"
#include "shaders/PgDiffEmisRimFP_pl_sl_li.h"
#include "shaders/PgDiffEmisRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffEmisRimFP_sl.h"
#include "shaders/PgDiffEmisRimFP_sl_modern.h"
#include "shaders/PgDiffEmisRimFP_sl_li.h"
#include "shaders/PgDiffEmisRimFP_sl_li_modern.h"
#include "shaders/PgDiffFP.h"
#include "shaders/PgDiffFP_modern.h"
#include "shaders/PgDiffFP_pl.h"
#include "shaders/PgDiffFP_pl_modern.h"
#include "shaders/PgDiffFP_pl_li.h"
#include "shaders/PgDiffFP_pl_li_modern.h"
#include "shaders/PgDiffFP_pl_sl.h"
#include "shaders/PgDiffFP_pl_sl_modern.h"
#include "shaders/PgDiffFP_pl_sl_li.h"
#include "shaders/PgDiffFP_pl_sl_li_modern.h"
#include "shaders/PgDiffFP_sl.h"
#include "shaders/PgDiffFP_sl_modern.h"
#include "shaders/PgDiffFP_sl_li.h"
#include "shaders/PgDiffFP_sl_li_modern.h"
#include "shaders/PgDiffRimFP.h"
#include "shaders/PgDiffRimFP_modern.h"
#include "shaders/PgDiffRimFP_pl.h"
#include "shaders/PgDiffRimFP_pl_modern.h"
#include "shaders/PgDiffRimFP_pl_li.h"
#include "shaders/PgDiffRimFP_pl_li_modern.h"
#include "shaders/PgDiffRimFP_pl_sl.h"
#include "shaders/PgDiffRimFP_pl_sl_modern.h"
#include "shaders/PgDiffRimFP_pl_sl_li.h"
#include "shaders/PgDiffRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffRimFP_sl.h"
#include "shaders/PgDiffRimFP_sl_modern.h"
#include "shaders/PgDiffRimFP_sl_li.h"
#include "shaders/PgDiffRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecAmbOccFP.h"
#include "shaders/PgDiffSpecAmbOccFP_modern.h"
#include "shaders/PgDiffSpecAmbOccFP_pl.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecAmbOccFP_sl.h"
#include "shaders/PgDiffSpecAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP.h"
#include "shaders/PgDiffSpecAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecFP.h"
#include "shaders/PgDiffSpecFP_modern.h"
#include "shaders/PgDiffSpecFP_pl.h"
#include "shaders/PgDiffSpecFP_pl_modern.h"
#include "shaders/PgDiffSpecFP_pl_li.h"
#include "shaders/PgDiffSpecFP_pl_li_modern.h"
#include "shaders/PgDiffSpecFP_pl_sl.h"
#include "shaders/PgDiffSpecFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecFP_pl_sl_li.h"
#include "shaders/PgDiffSpecFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecFP_sl.h"
#include "shaders/PgDiffSpecFP_sl_modern.h"
#include "shaders/PgDiffSpecFP_sl_li.h"
#include "shaders/PgDiffSpecFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_sl.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecMetalAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecMetalAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalFP.h"
#include "shaders/PgDiffSpecMetalFP_modern.h"
#include "shaders/PgDiffSpecMetalFP_pl.h"
#include "shaders/PgDiffSpecMetalFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalFP_pl_li.h"
#include "shaders/PgDiffSpecMetalFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalFP_sl.h"
#include "shaders/PgDiffSpecMetalFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalFP_sl_li.h"
#include "shaders/PgDiffSpecMetalFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_sl.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecMetalNormAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecMetalNormAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP.h"
#include "shaders/PgDiffSpecMetalNormRimFP_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_li.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalNormRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP_sl.h"
#include "shaders/PgDiffSpecMetalNormRimFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalNormRimFP_sl_li.h"
#include "shaders/PgDiffSpecMetalNormRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalRimFP.h"
#include "shaders/PgDiffSpecMetalRimFP_modern.h"
#include "shaders/PgDiffSpecMetalRimFP_pl.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_modern.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_li.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_sl.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecMetalRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecMetalRimFP_sl.h"
#include "shaders/PgDiffSpecMetalRimFP_sl_modern.h"
#include "shaders/PgDiffSpecMetalRimFP_sl_li.h"
#include "shaders/PgDiffSpecMetalRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecNormAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecNormRimFP.h"
#include "shaders/PgDiffSpecNormRimFP_modern.h"
#include "shaders/PgDiffSpecNormRimFP_pl.h"
#include "shaders/PgDiffSpecNormRimFP_pl_modern.h"
#include "shaders/PgDiffSpecNormRimFP_pl_li.h"
#include "shaders/PgDiffSpecNormRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecNormRimFP_pl_sl.h"
#include "shaders/PgDiffSpecNormRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecNormRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecNormRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecNormRimFP_sl.h"
#include "shaders/PgDiffSpecNormRimFP_sl_modern.h"
#include "shaders/PgDiffSpecNormRimFP_sl_li.h"
#include "shaders/PgDiffSpecNormRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP.h"
#include "shaders/PgDiffSpecReflAmbOccFP_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP_sl.h"
#include "shaders/PgDiffSpecReflAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecReflAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecReflAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecReflAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflFP.h"
#include "shaders/PgDiffSpecReflFP_modern.h"
#include "shaders/PgDiffSpecReflFP_pl.h"
#include "shaders/PgDiffSpecReflFP_pl_modern.h"
#include "shaders/PgDiffSpecReflFP_pl_li.h"
#include "shaders/PgDiffSpecReflFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflFP_pl_sl.h"
#include "shaders/PgDiffSpecReflFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflFP_sl.h"
#include "shaders/PgDiffSpecReflFP_sl_modern.h"
#include "shaders/PgDiffSpecReflFP_sl_li.h"
#include "shaders/PgDiffSpecReflFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_sl.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecReflNormAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecReflNormAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormFP.h"
#include "shaders/PgDiffSpecReflNormFP_modern.h"
#include "shaders/PgDiffSpecReflNormFP_pl.h"
#include "shaders/PgDiffSpecReflNormFP_pl_modern.h"
#include "shaders/PgDiffSpecReflNormFP_pl_li.h"
#include "shaders/PgDiffSpecReflNormFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflNormFP_pl_sl.h"
#include "shaders/PgDiffSpecReflNormFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflNormFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflNormFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormFP_sl.h"
#include "shaders/PgDiffSpecReflNormFP_sl_modern.h"
#include "shaders/PgDiffSpecReflNormFP_sl_li.h"
#include "shaders/PgDiffSpecReflNormFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP.h"
#include "shaders/PgDiffSpecReflNormRimFP_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_li.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_sl.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflNormRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP_sl.h"
#include "shaders/PgDiffSpecReflNormRimFP_sl_modern.h"
#include "shaders/PgDiffSpecReflNormRimFP_sl_li.h"
#include "shaders/PgDiffSpecReflNormRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecReflRimFP.h"
#include "shaders/PgDiffSpecReflRimFP_modern.h"
#include "shaders/PgDiffSpecReflRimFP_pl.h"
#include "shaders/PgDiffSpecReflRimFP_pl_modern.h"
#include "shaders/PgDiffSpecReflRimFP_pl_li.h"
#include "shaders/PgDiffSpecReflRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecReflRimFP_pl_sl.h"
#include "shaders/PgDiffSpecReflRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecReflRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecReflRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecReflRimFP_sl.h"
#include "shaders/PgDiffSpecReflRimFP_sl_modern.h"
#include "shaders/PgDiffSpecReflRimFP_sl_li.h"
#include "shaders/PgDiffSpecReflRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecRimFP.h"
#include "shaders/PgDiffSpecRimFP_modern.h"
#include "shaders/PgDiffSpecRimFP_pl.h"
#include "shaders/PgDiffSpecRimFP_pl_modern.h"
#include "shaders/PgDiffSpecRimFP_pl_li.h"
#include "shaders/PgDiffSpecRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecRimFP_pl_sl.h"
#include "shaders/PgDiffSpecRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecRimFP_sl.h"
#include "shaders/PgDiffSpecRimFP_sl_modern.h"
#include "shaders/PgDiffSpecRimFP_sl_li.h"
#include "shaders/PgDiffSpecRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_sl.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecSSSAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecSSSAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSFP.h"
#include "shaders/PgDiffSpecSSSFP_modern.h"
#include "shaders/PgDiffSpecSSSFP_pl.h"
#include "shaders/PgDiffSpecSSSFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSFP_pl_li.h"
#include "shaders/PgDiffSpecSSSFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSFP_sl.h"
#include "shaders/PgDiffSpecSSSFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSFP_sl_li.h"
#include "shaders/PgDiffSpecSSSFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_li.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_sl.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_sl_li.h"
#include "shaders/PgDiffSpecSSSNormAmbOccFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_li.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_sl.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_sl_li.h"
#include "shaders/PgDiffSpecSSSNormAmbOccRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormFP.h"
#include "shaders/PgDiffSpecSSSNormFP_modern.h"
#include "shaders/PgDiffSpecSSSNormFP_pl.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_li.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSNormFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormFP_sl.h"
#include "shaders/PgDiffSpecSSSNormFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormFP_sl_li.h"
#include "shaders/PgDiffSpecSSSNormFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP.h"
#include "shaders/PgDiffSpecSSSNormRimFP_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_li.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSNormRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP_sl.h"
#include "shaders/PgDiffSpecSSSNormRimFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSNormRimFP_sl_li.h"
#include "shaders/PgDiffSpecSSSNormRimFP_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSRimFP.h"
#include "shaders/PgDiffSpecSSSRimFP_modern.h"
#include "shaders/PgDiffSpecSSSRimFP_pl.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_modern.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_li.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_li_modern.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_sl.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_sl_modern.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_sl_li.h"
#include "shaders/PgDiffSpecSSSRimFP_pl_sl_li_modern.h"
#include "shaders/PgDiffSpecSSSRimFP_sl.h"
#include "shaders/PgDiffSpecSSSRimFP_sl_modern.h"
#include "shaders/PgDiffSpecSSSRimFP_sl_li.h"
#include "shaders/PgDiffSpecSSSRimFP_sl_li_modern.h"
#include "shaders/PgFastFP.h"
#include "shaders/PgFastFP_modern.h"
#include "shaders/PgFastFP_pl.h"
#include "shaders/PgFastFP_pl_modern.h"
#include "shaders/PgFastFP_pl_li.h"
#include "shaders/PgFastFP_pl_li_modern.h"
#include "shaders/PgFastFP_pl_sl.h"
#include "shaders/PgFastFP_pl_sl_modern.h"
#include "shaders/PgFastFP_pl_sl_li.h"
#include "shaders/PgFastFP_pl_sl_li_modern.h"
#include "shaders/PgFastFP_sl.h"
#include "shaders/PgFastFP_sl_modern.h"
#include "shaders/PgFastFP_sl_li.h"
#include "shaders/PgFastFP_sl_li_modern.h"
#include "shaders/PgScrubLightFP.h"
#include "shaders/PgScrubLightFP_modern.h"
#include "shaders/PgTerrainMeshFP.h"
#include "shaders/PgTerrainMeshFP_modern.h"
#include "shaders/PgTerrainMeshFP1D.h"
#include "shaders/PgTerrainMeshFP1D_modern.h"
#include "shaders/PgTerrainMeshFP1D_pl.h"
#include "shaders/PgTerrainMeshFP1D_pl_modern.h"
#include "shaders/PgTerrainMeshFP1D_pl_li.h"
#include "shaders/PgTerrainMeshFP1D_pl_li_modern.h"
#include "shaders/PgTerrainMeshFP1D_pl_sl.h"
#include "shaders/PgTerrainMeshFP1D_pl_sl_modern.h"
#include "shaders/PgTerrainMeshFP1D_pl_sl_li.h"
#include "shaders/PgTerrainMeshFP1D_pl_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP1D_sl.h"
#include "shaders/PgTerrainMeshFP1D_sl_modern.h"
#include "shaders/PgTerrainMeshFP1D_sl_li.h"
#include "shaders/PgTerrainMeshFP1D_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP2D.h"
#include "shaders/PgTerrainMeshFP2D_modern.h"
#include "shaders/PgTerrainMeshFP2D_pl.h"
#include "shaders/PgTerrainMeshFP2D_pl_modern.h"
#include "shaders/PgTerrainMeshFP2D_pl_li.h"
#include "shaders/PgTerrainMeshFP2D_pl_li_modern.h"
#include "shaders/PgTerrainMeshFP2D_pl_sl.h"
#include "shaders/PgTerrainMeshFP2D_pl_sl_modern.h"
#include "shaders/PgTerrainMeshFP2D_pl_sl_li.h"
#include "shaders/PgTerrainMeshFP2D_pl_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP2D_sl.h"
#include "shaders/PgTerrainMeshFP2D_sl_modern.h"
#include "shaders/PgTerrainMeshFP2D_sl_li.h"
#include "shaders/PgTerrainMeshFP2D_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP3D.h"
#include "shaders/PgTerrainMeshFP3D_modern.h"
#include "shaders/PgTerrainMeshFP3D_pl.h"
#include "shaders/PgTerrainMeshFP3D_pl_modern.h"
#include "shaders/PgTerrainMeshFP3D_pl_li.h"
#include "shaders/PgTerrainMeshFP3D_pl_li_modern.h"
#include "shaders/PgTerrainMeshFP3D_pl_sl.h"
#include "shaders/PgTerrainMeshFP3D_pl_sl_modern.h"
#include "shaders/PgTerrainMeshFP3D_pl_sl_li.h"
#include "shaders/PgTerrainMeshFP3D_pl_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP3D_sl.h"
#include "shaders/PgTerrainMeshFP3D_sl_modern.h"
#include "shaders/PgTerrainMeshFP3D_sl_li.h"
#include "shaders/PgTerrainMeshFP3D_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP4D.h"
#include "shaders/PgTerrainMeshFP4D_modern.h"
#include "shaders/PgTerrainMeshFP4D_pl.h"
#include "shaders/PgTerrainMeshFP4D_pl_modern.h"
#include "shaders/PgTerrainMeshFP4D_pl_li.h"
#include "shaders/PgTerrainMeshFP4D_pl_li_modern.h"
#include "shaders/PgTerrainMeshFP4D_pl_sl.h"
#include "shaders/PgTerrainMeshFP4D_pl_sl_modern.h"
#include "shaders/PgTerrainMeshFP4D_pl_sl_li.h"
#include "shaders/PgTerrainMeshFP4D_pl_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP4D_sl.h"
#include "shaders/PgTerrainMeshFP4D_sl_modern.h"
#include "shaders/PgTerrainMeshFP4D_sl_li.h"
#include "shaders/PgTerrainMeshFP4D_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP_pl.h"
#include "shaders/PgTerrainMeshFP_pl_modern.h"
#include "shaders/PgTerrainMeshFP_pl_li.h"
#include "shaders/PgTerrainMeshFP_pl_li_modern.h"
#include "shaders/PgTerrainMeshFP_pl_sl.h"
#include "shaders/PgTerrainMeshFP_pl_sl_modern.h"
#include "shaders/PgTerrainMeshFP_pl_sl_li.h"
#include "shaders/PgTerrainMeshFP_pl_sl_li_modern.h"
#include "shaders/PgTerrainMeshFP_sl.h"
#include "shaders/PgTerrainMeshFP_sl_modern.h"
#include "shaders/PgTerrainMeshFP_sl_li.h"
#include "shaders/PgTerrainMeshFP_sl_li_modern.h"
#include "shaders/PgUnderwaterFP.h"
#include "shaders/PgWaterFP.h"
#include "shaders/PgWaterFPDWE.h"
#include "shaders/PgWaterFP_LI.h"
#include "shaders/PgWaterFPbl.h"
#include "shaders/PgScrubLightFP_pl.h"
#include "shaders/PgScrubLightFP_pl_modern.h"
#include "shaders/PgScrubLightFP_pl_li.h"
#include "shaders/PgScrubLightFP_pl_li_modern.h"
#include "shaders/PgScrubLightFP_pl_sl.h"
#include "shaders/PgScrubLightFP_pl_sl_modern.h"
#include "shaders/PgScrubLightFP_pl_sl_li.h"
#include "shaders/PgScrubLightFP_pl_sl_li_modern.h"
#include "shaders/PgScrubLightFP_sl.h"
#include "shaders/PgScrubLightFP_sl_modern.h"
#include "shaders/PgScrubLightFP_sl_li.h"
#include "shaders/PgScrubLightFP_sl_li_modern.h"

namespace mc2vr::hooks::shader_reimpl {

namespace {

struct Entry {
    uint32_t original;     // FNV-1a of the original's bytecode
    const char *name;
    const uint32_t *code;    // ours (the classic shading: the original's math)
    const uint32_t *modern;  // ours in the modern shading (shading=modern), or nullptr (none: `code`)
    bool adds_camera_pos = false;  // kAddsCameraPos (below)
    uint32_t created = 0;         // substitutions so far
    bool failed = false;          // the device refused ours
    const uint32_t *last_used = nullptr;  // what was created for it last (ours, maybe texel-scaled)
    bool last_modern = false;             // ... and whether that was the modern shading
};

// The entry's modern shader takes `float3 cameraPos : register(c92)`, which
// the original's interface lacks (every original with a cameraPos has it at
// c92, and these use no part of c92). The game uploads it anyway, per eye:
// it resolves a shader's constants by name from the object it created (ours),
// and its pass uploads cameraPos wherever the shader declares one (live,
// 2026-11-16: every such draw). tools/shader_iface.py --modern allows exactly
// this addition for exactly these entries.
constexpr bool kAddsCameraPos = true;

// One line per reimplemented shader; docs/shaders.md lists them.
Entry g_entries[] = {
    // PgBillboardTreeFP and PgBillboardTreeFP_fade share one bytecode (shader3.bin@0x2ff0, @0x3790).
    {0xeacf64f8u, "PgBillboardTreeFP (shader3.bin@0x2ff0)", mc2vr::shaders::PgBillboardTreeFP, mc2vr::shaders::PgBillboardTreeFP_modern, kAddsCameraPos},
    {0xdeef11a9u, "PgDecal2FP (shader3.bin@0xa2a0)", mc2vr::shaders::PgDecal2FP, mc2vr::shaders::PgDecal2FP_modern, kAddsCameraPos},
    {0x8360631bu, "PgDecal2FP_pl (shader3.bin@0xa840)", mc2vr::shaders::PgDecal2FP_pl, mc2vr::shaders::PgDecal2FP_pl_modern, kAddsCameraPos},
    {0xe40c452eu, "PgDecal2FP_pl_li (shader3.bin@0xb800)", mc2vr::shaders::PgDecal2FP_pl_li, mc2vr::shaders::PgDecal2FP_pl_li_modern, kAddsCameraPos},
    {0x6751d4d3u, "PgDecal2FP_pl_sl (shader3.bin@0xc210)", mc2vr::shaders::PgDecal2FP_pl_sl, mc2vr::shaders::PgDecal2FP_pl_sl_modern, kAddsCameraPos},
    {0x4552f636u, "PgDecal2FP_pl_sl_li (shader3.bin@0xddd0)", mc2vr::shaders::PgDecal2FP_pl_sl_li, mc2vr::shaders::PgDecal2FP_pl_sl_li_modern, kAddsCameraPos},
    {0x15cf77adu, "PgDecal2FP_sl (shader3.bin@0xeed0)", mc2vr::shaders::PgDecal2FP_sl, mc2vr::shaders::PgDecal2FP_sl_modern, kAddsCameraPos},
    {0xe651a6c7u, "PgDecal2FP_sl_li (shader3.bin@0x10070)", mc2vr::shaders::PgDecal2FP_sl_li, mc2vr::shaders::PgDecal2FP_sl_li_modern, kAddsCameraPos},
    // PgDiffRefractFP and its 6 light variants share one bytecode (shader3.bin@0x4aa40 .. @0x4c4e0).
    {0xdc91abe9u, "PgDiffRefractFP (shader3.bin@0x4aa40)", mc2vr::shaders::PgDiffRefractFP, nullptr},
    // PgDiffRefractNormFP and its 6 light variants share one bytecode (shader3.bin@0x4c950 .. @0x4ea50).
    {0x4a771f15u, "PgDiffRefractNormFP (shader3.bin@0x4c950)", mc2vr::shaders::PgDiffRefractNormFP, nullptr},
    {0xb45eaaccu, "PgDiffSpecNormAmbOccFP (shader3.bin@0xd9c70)", mc2vr::shaders::PgDiffSpecNormAmbOccFP, mc2vr::shaders::PgDiffSpecNormAmbOccFP_modern},
    {0x700f33e6u, "PgDiffSpecNormAmbOccFP_pl (shader3.bin@0xda8a0)", mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl, mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_modern},
    {0xb11bb21fu, "PgDiffSpecNormAmbOccFP_pl_li (shader3.bin@0xdc7f0)", mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_li_modern},
    {0xf40d81d0u, "PgDiffSpecNormAmbOccFP_pl_sl (shader3.bin@0xddc10)", mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_sl_modern},
    {0xee834b1fu, "PgDiffSpecNormAmbOccFP_pl_sl_li (shader3.bin@0xe0ba0)", mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecNormAmbOccFP_pl_sl_li_modern},
    {0x44ad8ebau, "PgDiffSpecNormAmbOccFP_sl (shader3.bin@0xe2930)", mc2vr::shaders::PgDiffSpecNormAmbOccFP_sl, mc2vr::shaders::PgDiffSpecNormAmbOccFP_sl_modern},
    {0xea5d97d6u, "PgDiffSpecNormAmbOccFP_sl_li (shader3.bin@0xe4580)", mc2vr::shaders::PgDiffSpecNormAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecNormAmbOccFP_sl_li_modern},
    {0x3591a5e2u, "PgDiffSpecNormFP (shader3.bin@0xf1bf0)", mc2vr::shaders::PgDiffSpecNormFP, mc2vr::shaders::PgDiffSpecNormFP_modern},
    {0xb0a3f572u, "PgDiffSpecNormFP_pl (shader3.bin@0xf27f0)", mc2vr::shaders::PgDiffSpecNormFP_pl, mc2vr::shaders::PgDiffSpecNormFP_pl_modern},
    {0x6c3879dbu, "PgDiffSpecNormFP_pl_li (shader3.bin@0xf4710)", mc2vr::shaders::PgDiffSpecNormFP_pl_li, mc2vr::shaders::PgDiffSpecNormFP_pl_li_modern},
    {0xa157a03cu, "PgDiffSpecNormFP_pl_sl (shader3.bin@0xf5b10)", mc2vr::shaders::PgDiffSpecNormFP_pl_sl, mc2vr::shaders::PgDiffSpecNormFP_pl_sl_modern},
    {0x7899734bu, "PgDiffSpecNormFP_pl_sl_li (shader3.bin@0xf8a70)", mc2vr::shaders::PgDiffSpecNormFP_pl_sl_li, mc2vr::shaders::PgDiffSpecNormFP_pl_sl_li_modern},
    {0x41cb2e02u, "PgDiffSpecNormFP_sl (shader3.bin@0xfa7d0)", mc2vr::shaders::PgDiffSpecNormFP_sl, mc2vr::shaders::PgDiffSpecNormFP_sl_modern},
    {0x1881c562u, "PgDiffSpecNormFP_sl_li (shader3.bin@0xfc3f0)", mc2vr::shaders::PgDiffSpecNormFP_sl_li, mc2vr::shaders::PgDiffSpecNormFP_sl_li_modern},
    {0xa27d6bfau, "PgDiffSpecMetalNormFP (shader3.bin@0xb5270)", mc2vr::shaders::PgDiffSpecMetalNormFP, mc2vr::shaders::PgDiffSpecMetalNormFP_modern},
    {0x278e01a2u, "PgDiffSpecMetalNormFP_pl (shader3.bin@0xb5fc0)", mc2vr::shaders::PgDiffSpecMetalNormFP_pl, mc2vr::shaders::PgDiffSpecMetalNormFP_pl_modern},
    {0x34dc487eu, "PgDiffSpecMetalNormFP_pl_li (shader3.bin@0xb8020)", mc2vr::shaders::PgDiffSpecMetalNormFP_pl_li, mc2vr::shaders::PgDiffSpecMetalNormFP_pl_li_modern},
    {0x60dbcb70u, "PgDiffSpecMetalNormFP_pl_sl (shader3.bin@0xb9540)", mc2vr::shaders::PgDiffSpecMetalNormFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalNormFP_pl_sl_modern},
    {0x065fb9a4u, "PgDiffSpecMetalNormFP_pl_sl_li (shader3.bin@0xbc5e0)", mc2vr::shaders::PgDiffSpecMetalNormFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalNormFP_pl_sl_li_modern},
    {0x483bd668u, "PgDiffSpecMetalNormFP_sl (shader3.bin@0xbe480)", mc2vr::shaders::PgDiffSpecMetalNormFP_sl, mc2vr::shaders::PgDiffSpecMetalNormFP_sl_modern},
    {0xb7e6aafdu, "PgDiffSpecMetalNormFP_sl_li (shader3.bin@0xc01e0)", mc2vr::shaders::PgDiffSpecMetalNormFP_sl_li, mc2vr::shaders::PgDiffSpecMetalNormFP_sl_li_modern},
    {0xeca746f7u, "PgDiffAmbOccFP (shader3.bin@0x11570)", mc2vr::shaders::PgDiffAmbOccFP, mc2vr::shaders::PgDiffAmbOccFP_modern, kAddsCameraPos},
    {0xa32fdbf1u, "PgDiffAmbOccFP_pl (shader3.bin@0x11e50)", mc2vr::shaders::PgDiffAmbOccFP_pl, mc2vr::shaders::PgDiffAmbOccFP_pl_modern, kAddsCameraPos},
    {0x1bca4bd8u, "PgDiffAmbOccFP_pl_li (shader3.bin@0x13130)", mc2vr::shaders::PgDiffAmbOccFP_pl_li, mc2vr::shaders::PgDiffAmbOccFP_pl_li_modern, kAddsCameraPos},
    {0xf51899f2u, "PgDiffAmbOccFP_pl_sl (shader3.bin@0x13e50)", mc2vr::shaders::PgDiffAmbOccFP_pl_sl, mc2vr::shaders::PgDiffAmbOccFP_pl_sl_modern, kAddsCameraPos},
    {0x0cdf5dc1u, "PgDiffAmbOccFP_pl_sl_li (shader3.bin@0x15d30)", mc2vr::shaders::PgDiffAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffAmbOccFP_pl_sl_li_modern, kAddsCameraPos},
    {0x8d5debcdu, "PgDiffAmbOccFP_sl (shader3.bin@0x17150)", mc2vr::shaders::PgDiffAmbOccFP_sl, mc2vr::shaders::PgDiffAmbOccFP_sl_modern, kAddsCameraPos},
    {0x0a2900c5u, "PgDiffAmbOccFP_sl_li (shader3.bin@0x18610)", mc2vr::shaders::PgDiffAmbOccFP_sl_li, mc2vr::shaders::PgDiffAmbOccFP_sl_li_modern, kAddsCameraPos},
    {0x98043426u, "PgDiffAmbOccRimFP (shader3.bin@0x195e0)", mc2vr::shaders::PgDiffAmbOccRimFP, mc2vr::shaders::PgDiffAmbOccRimFP_modern},
    {0x298e7d23u, "PgDiffAmbOccRimFP_pl (shader3.bin@0x19f70)", mc2vr::shaders::PgDiffAmbOccRimFP_pl, mc2vr::shaders::PgDiffAmbOccRimFP_pl_modern},
    {0x5cfce082u, "PgDiffAmbOccRimFP_pl_li (shader3.bin@0x1b2e0)", mc2vr::shaders::PgDiffAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffAmbOccRimFP_pl_li_modern},
    {0xf73c699cu, "PgDiffAmbOccRimFP_pl_sl (shader3.bin@0x1c090)", mc2vr::shaders::PgDiffAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffAmbOccRimFP_pl_sl_modern},
    {0x8a3aff33u, "PgDiffAmbOccRimFP_pl_sl_li (shader3.bin@0x1e000)", mc2vr::shaders::PgDiffAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffAmbOccRimFP_pl_sl_li_modern},
    {0x3bda0918u, "PgDiffAmbOccRimFP_sl (shader3.bin@0x1f4d0)", mc2vr::shaders::PgDiffAmbOccRimFP_sl, mc2vr::shaders::PgDiffAmbOccRimFP_sl_modern},
    {0x4ffff1bdu, "PgDiffAmbOccRimFP_sl_li (shader3.bin@0x20a20)", mc2vr::shaders::PgDiffAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffAmbOccRimFP_sl_li_modern},
    {0xc332796eu, "PgDiffEmisAmbOccFP (shader3.bin@0x21a80)", mc2vr::shaders::PgDiffEmisAmbOccFP, mc2vr::shaders::PgDiffEmisAmbOccFP_modern, kAddsCameraPos},
    {0x63d0e272u, "PgDiffEmisAmbOccFP_pl (shader3.bin@0x223b0)", mc2vr::shaders::PgDiffEmisAmbOccFP_pl, mc2vr::shaders::PgDiffEmisAmbOccFP_pl_modern, kAddsCameraPos},
    {0xc3d1e0e3u, "PgDiffEmisAmbOccFP_pl_li (shader3.bin@0x236e0)", mc2vr::shaders::PgDiffEmisAmbOccFP_pl_li, mc2vr::shaders::PgDiffEmisAmbOccFP_pl_li_modern, kAddsCameraPos},
    {0x2ec2d4c2u, "PgDiffEmisAmbOccFP_pl_sl (shader3.bin@0x24450)", mc2vr::shaders::PgDiffEmisAmbOccFP_pl_sl, mc2vr::shaders::PgDiffEmisAmbOccFP_pl_sl_modern, kAddsCameraPos},
    {0xa532cae9u, "PgDiffEmisAmbOccFP_pl_sl_li (shader3.bin@0x26380)", mc2vr::shaders::PgDiffEmisAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffEmisAmbOccFP_pl_sl_li_modern, kAddsCameraPos},
    {0x1854e14du, "PgDiffEmisAmbOccFP_sl (shader3.bin@0x277f0)", mc2vr::shaders::PgDiffEmisAmbOccFP_sl, mc2vr::shaders::PgDiffEmisAmbOccFP_sl_modern, kAddsCameraPos},
    {0x837752c1u, "PgDiffEmisAmbOccFP_sl_li (shader3.bin@0x28d00)", mc2vr::shaders::PgDiffEmisAmbOccFP_sl_li, mc2vr::shaders::PgDiffEmisAmbOccFP_sl_li_modern, kAddsCameraPos},
    {0xac88c72eu, "PgDiffEmisAmbOccRimFP (shader3.bin@0x29d20)", mc2vr::shaders::PgDiffEmisAmbOccRimFP, mc2vr::shaders::PgDiffEmisAmbOccRimFP_modern},
    {0x8f005dccu, "PgDiffEmisAmbOccRimFP_pl (shader3.bin@0x2a700)", mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl, mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_modern},
    {0x178065b5u, "PgDiffEmisAmbOccRimFP_pl_li (shader3.bin@0x2bac0)", mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_li_modern},
    {0x36821f33u, "PgDiffEmisAmbOccRimFP_pl_sl (shader3.bin@0x2c8c0)", mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_sl_modern},
    {0x55c9af4cu, "PgDiffEmisAmbOccRimFP_pl_sl_li (shader3.bin@0x2e880)", mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffEmisAmbOccRimFP_pl_sl_li_modern},
    {0xf5ac540cu, "PgDiffEmisAmbOccRimFP_sl (shader3.bin@0x2fda0)", mc2vr::shaders::PgDiffEmisAmbOccRimFP_sl, mc2vr::shaders::PgDiffEmisAmbOccRimFP_sl_modern},
    {0x45be32d5u, "PgDiffEmisAmbOccRimFP_sl_li (shader3.bin@0x31340)", mc2vr::shaders::PgDiffEmisAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffEmisAmbOccRimFP_sl_li_modern},
    {0x7a1955d0u, "PgDiffEmisFP (shader3.bin@0x323f0)", mc2vr::shaders::PgDiffEmisFP, mc2vr::shaders::PgDiffEmisFP_modern, kAddsCameraPos},
    {0x44d74994u, "PgDiffEmisFP_pl (shader3.bin@0x32d00)", mc2vr::shaders::PgDiffEmisFP_pl, mc2vr::shaders::PgDiffEmisFP_pl_modern, kAddsCameraPos},
    {0x226c2c2du, "PgDiffEmisFP_pl_li (shader3.bin@0x34000)", mc2vr::shaders::PgDiffEmisFP_pl_li, mc2vr::shaders::PgDiffEmisFP_pl_li_modern, kAddsCameraPos},
    {0xf0526d38u, "PgDiffEmisFP_pl_sl (shader3.bin@0x34d40)", mc2vr::shaders::PgDiffEmisFP_pl_sl, mc2vr::shaders::PgDiffEmisFP_pl_sl_modern, kAddsCameraPos},
    {0x3f4d3a97u, "PgDiffEmisFP_pl_sl_li (shader3.bin@0x36c40)", mc2vr::shaders::PgDiffEmisFP_pl_sl_li, mc2vr::shaders::PgDiffEmisFP_pl_sl_li_modern, kAddsCameraPos},
    {0x0a3e33dbu, "PgDiffEmisFP_sl (shader3.bin@0x38090)", mc2vr::shaders::PgDiffEmisFP_sl, mc2vr::shaders::PgDiffEmisFP_sl_modern, kAddsCameraPos},
    {0x5eb0730bu, "PgDiffEmisFP_sl_li (shader3.bin@0x39570)", mc2vr::shaders::PgDiffEmisFP_sl_li, mc2vr::shaders::PgDiffEmisFP_sl_li_modern, kAddsCameraPos},
    {0x3ec91ae0u, "PgDiffEmisRimFP (shader3.bin@0x3a560)", mc2vr::shaders::PgDiffEmisRimFP, mc2vr::shaders::PgDiffEmisRimFP_modern},
    {0xd02b0322u, "PgDiffEmisRimFP_pl (shader3.bin@0x3af20)", mc2vr::shaders::PgDiffEmisRimFP_pl, mc2vr::shaders::PgDiffEmisRimFP_pl_modern},
    {0xb659a47bu, "PgDiffEmisRimFP_pl_li (shader3.bin@0x3c2b0)", mc2vr::shaders::PgDiffEmisRimFP_pl_li, mc2vr::shaders::PgDiffEmisRimFP_pl_li_modern},
    {0x57aa3711u, "PgDiffEmisRimFP_pl_sl (shader3.bin@0x3d080)", mc2vr::shaders::PgDiffEmisRimFP_pl_sl, mc2vr::shaders::PgDiffEmisRimFP_pl_sl_modern},
    {0x6c78b846u, "PgDiffEmisRimFP_pl_sl_li (shader3.bin@0x3f010)", mc2vr::shaders::PgDiffEmisRimFP_pl_sl_li, mc2vr::shaders::PgDiffEmisRimFP_pl_sl_li_modern},
    {0xbcd156f6u, "PgDiffEmisRimFP_sl (shader3.bin@0x40500)", mc2vr::shaders::PgDiffEmisRimFP_sl, mc2vr::shaders::PgDiffEmisRimFP_sl_modern},
    {0xcd643023u, "PgDiffEmisRimFP_sl_li (shader3.bin@0x41a80)", mc2vr::shaders::PgDiffEmisRimFP_sl_li, mc2vr::shaders::PgDiffEmisRimFP_sl_li_modern},
    {0xdba94c85u, "PgDiffFP (shader3.bin@0x42b00)", mc2vr::shaders::PgDiffFP, mc2vr::shaders::PgDiffFP_modern, kAddsCameraPos},
    {0x416cf8e7u, "PgDiffFP_pl (shader3.bin@0x433c0)", mc2vr::shaders::PgDiffFP_pl, mc2vr::shaders::PgDiffFP_pl_modern, kAddsCameraPos},
    {0xd1d1c3c6u, "PgDiffFP_pl_li (shader3.bin@0x44670)", mc2vr::shaders::PgDiffFP_pl_li, mc2vr::shaders::PgDiffFP_pl_li_modern, kAddsCameraPos},
    {0x7ebfec6cu, "PgDiffFP_pl_sl (shader3.bin@0x45360)", mc2vr::shaders::PgDiffFP_pl_sl, mc2vr::shaders::PgDiffFP_pl_sl_modern, kAddsCameraPos},
    {0x3a0f09c3u, "PgDiffFP_pl_sl_li (shader3.bin@0x47210)", mc2vr::shaders::PgDiffFP_pl_sl_li, mc2vr::shaders::PgDiffFP_pl_sl_li_modern, kAddsCameraPos},
    {0xaa548d97u, "PgDiffFP_sl (shader3.bin@0x48610)", mc2vr::shaders::PgDiffFP_sl, mc2vr::shaders::PgDiffFP_sl_modern, kAddsCameraPos},
    {0x1e9c298bu, "PgDiffFP_sl_li (shader3.bin@0x49aa0)", mc2vr::shaders::PgDiffFP_sl_li, mc2vr::shaders::PgDiffFP_sl_li_modern, kAddsCameraPos},
    {0x80b68050u, "PgDiffRimFP (shader3.bin@0x4efd0)", mc2vr::shaders::PgDiffRimFP, mc2vr::shaders::PgDiffRimFP_modern},
    {0x7b1b379du, "PgDiffRimFP_pl (shader3.bin@0x4f940)", mc2vr::shaders::PgDiffRimFP_pl, mc2vr::shaders::PgDiffRimFP_pl_modern},
    {0x8da31f2cu, "PgDiffRimFP_pl_li (shader3.bin@0x50c80)", mc2vr::shaders::PgDiffRimFP_pl_li, mc2vr::shaders::PgDiffRimFP_pl_li_modern},
    {0x5c460d1eu, "PgDiffRimFP_pl_sl (shader3.bin@0x51a00)", mc2vr::shaders::PgDiffRimFP_pl_sl, mc2vr::shaders::PgDiffRimFP_pl_sl_modern},
    {0xdc1ad7d9u, "PgDiffRimFP_pl_sl_li (shader3.bin@0x53940)", mc2vr::shaders::PgDiffRimFP_pl_sl_li, mc2vr::shaders::PgDiffRimFP_pl_sl_li_modern},
    {0xb6690446u, "PgDiffRimFP_sl (shader3.bin@0x54de0)", mc2vr::shaders::PgDiffRimFP_sl, mc2vr::shaders::PgDiffRimFP_sl_modern},
    {0x9a245c27u, "PgDiffRimFP_sl_li (shader3.bin@0x56310)", mc2vr::shaders::PgDiffRimFP_sl_li, mc2vr::shaders::PgDiffRimFP_sl_li_modern},
    {0xd63c7540u, "PgDiffSpecAmbOccFP (shader3.bin@0x57340)", mc2vr::shaders::PgDiffSpecAmbOccFP, mc2vr::shaders::PgDiffSpecAmbOccFP_modern},
    {0xf002d390u, "PgDiffSpecAmbOccFP_pl (shader3.bin@0x57df0)", mc2vr::shaders::PgDiffSpecAmbOccFP_pl, mc2vr::shaders::PgDiffSpecAmbOccFP_pl_modern},
    {0x4a3db6f2u, "PgDiffSpecAmbOccFP_pl_li (shader3.bin@0x59ba0)", mc2vr::shaders::PgDiffSpecAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecAmbOccFP_pl_li_modern},
    {0x0636160eu, "PgDiffSpecAmbOccFP_pl_sl (shader3.bin@0x5ae10)", mc2vr::shaders::PgDiffSpecAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecAmbOccFP_pl_sl_modern},
    {0xf1dfa78du, "PgDiffSpecAmbOccFP_pl_sl_li (shader3.bin@0x5dc00)", mc2vr::shaders::PgDiffSpecAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecAmbOccFP_pl_sl_li_modern},
    {0xf92a29c8u, "PgDiffSpecAmbOccFP_sl (shader3.bin@0x5f7f0)", mc2vr::shaders::PgDiffSpecAmbOccFP_sl, mc2vr::shaders::PgDiffSpecAmbOccFP_sl_modern},
    {0xb3141ba8u, "PgDiffSpecAmbOccFP_sl_li (shader3.bin@0x612a0)", mc2vr::shaders::PgDiffSpecAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecAmbOccFP_sl_li_modern},
    {0xb20645feu, "PgDiffSpecAmbOccRimFP (shader3.bin@0x62690)", mc2vr::shaders::PgDiffSpecAmbOccRimFP, mc2vr::shaders::PgDiffSpecAmbOccRimFP_modern},
    {0xee8b9cc4u, "PgDiffSpecAmbOccRimFP_pl (shader3.bin@0x631b0)", mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_modern},
    {0x91751dc2u, "PgDiffSpecAmbOccRimFP_pl_li (shader3.bin@0x64fb0)", mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_li_modern},
    {0xe96845beu, "PgDiffSpecAmbOccRimFP_pl_sl (shader3.bin@0x66270)", mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_sl_modern},
    {0x23cbedffu, "PgDiffSpecAmbOccRimFP_pl_sl_li (shader3.bin@0x690c0)", mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecAmbOccRimFP_pl_sl_li_modern},
    {0x2940a67au, "PgDiffSpecAmbOccRimFP_sl (shader3.bin@0x6ad00)", mc2vr::shaders::PgDiffSpecAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecAmbOccRimFP_sl_modern},
    {0x09bf3efeu, "PgDiffSpecAmbOccRimFP_sl_li (shader3.bin@0x6c810)", mc2vr::shaders::PgDiffSpecAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecAmbOccRimFP_sl_li_modern},
    {0xc59af710u, "PgDiffSpecFP (shader3.bin@0x6dc50)", mc2vr::shaders::PgDiffSpecFP, mc2vr::shaders::PgDiffSpecFP_modern},
    {0xc8c7030au, "PgDiffSpecFP_pl (shader3.bin@0x6e6d0)", mc2vr::shaders::PgDiffSpecFP_pl, mc2vr::shaders::PgDiffSpecFP_pl_modern},
    {0x70f215fcu, "PgDiffSpecFP_pl_li (shader3.bin@0x70450)", mc2vr::shaders::PgDiffSpecFP_pl_li, mc2vr::shaders::PgDiffSpecFP_pl_li_modern},
    {0x8d608554u, "PgDiffSpecFP_pl_sl (shader3.bin@0x71690)", mc2vr::shaders::PgDiffSpecFP_pl_sl, mc2vr::shaders::PgDiffSpecFP_pl_sl_modern},
    {0x90f4651bu, "PgDiffSpecFP_pl_sl_li (shader3.bin@0x74450)", mc2vr::shaders::PgDiffSpecFP_pl_sl_li, mc2vr::shaders::PgDiffSpecFP_pl_sl_li_modern},
    {0xa3ee0916u, "PgDiffSpecFP_sl (shader3.bin@0x76010)", mc2vr::shaders::PgDiffSpecFP_sl, mc2vr::shaders::PgDiffSpecFP_sl_modern},
    {0x0e587a5eu, "PgDiffSpecFP_sl_li (shader3.bin@0x77a90)", mc2vr::shaders::PgDiffSpecFP_sl_li, mc2vr::shaders::PgDiffSpecFP_sl_li_modern},
    {0x42d7d457u, "PgDiffSpecMetalAmbOccFP (shader3.bin@0x78e50)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_modern},
    {0x6ad49eefu, "PgDiffSpecMetalAmbOccFP_pl (shader3.bin@0x79a20)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_modern},
    {0x0f728813u, "PgDiffSpecMetalAmbOccFP_pl_li (shader3.bin@0x7b900)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_li_modern},
    {0x7f7ff474u, "PgDiffSpecMetalAmbOccFP_pl_sl (shader3.bin@0x7ccc0)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_sl_modern},
    {0x817c194eu, "PgDiffSpecMetalAmbOccFP_pl_sl_li (shader3.bin@0x7fbf0)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_pl_sl_li_modern},
    {0x55b5028fu, "PgDiffSpecMetalAmbOccFP_sl (shader3.bin@0x81910)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP_sl, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_sl_modern},
    {0x0c3bb9ffu, "PgDiffSpecMetalAmbOccFP_sl_li (shader3.bin@0x83500)", mc2vr::shaders::PgDiffSpecMetalAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecMetalAmbOccFP_sl_li_modern},
    {0x86759fffu, "PgDiffSpecMetalAmbOccRimFP (shader3.bin@0x84a20)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_modern},
    {0x10b18d6fu, "PgDiffSpecMetalAmbOccRimFP_pl (shader3.bin@0x85640)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_modern},
    {0x3bad7fffu, "PgDiffSpecMetalAmbOccRimFP_pl_li (shader3.bin@0x87560)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_li_modern},
    {0x7df9d9a1u, "PgDiffSpecMetalAmbOccRimFP_pl_sl (shader3.bin@0x88940)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_sl_modern},
    {0x3cf19aa9u, "PgDiffSpecMetalAmbOccRimFP_pl_sl_li (shader3.bin@0x8b8a0)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_pl_sl_li_modern},
    {0xab970e85u, "PgDiffSpecMetalAmbOccRimFP_sl (shader3.bin@0x8d600)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_sl_modern},
    {0xa504b6a0u, "PgDiffSpecMetalAmbOccRimFP_sl_li (shader3.bin@0x8f220)", mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecMetalAmbOccRimFP_sl_li_modern},
    {0x52700b2fu, "PgDiffSpecMetalFP (shader3.bin@0x90780)", mc2vr::shaders::PgDiffSpecMetalFP, mc2vr::shaders::PgDiffSpecMetalFP_modern},
    {0x4ad998f9u, "PgDiffSpecMetalFP_pl (shader3.bin@0x91320)", mc2vr::shaders::PgDiffSpecMetalFP_pl, mc2vr::shaders::PgDiffSpecMetalFP_pl_modern},
    {0xdc29f351u, "PgDiffSpecMetalFP_pl_li (shader3.bin@0x931e0)", mc2vr::shaders::PgDiffSpecMetalFP_pl_li, mc2vr::shaders::PgDiffSpecMetalFP_pl_li_modern},
    {0x38f6827au, "PgDiffSpecMetalFP_pl_sl (shader3.bin@0x94570)", mc2vr::shaders::PgDiffSpecMetalFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalFP_pl_sl_modern},
    {0x9a891124u, "PgDiffSpecMetalFP_pl_sl_li (shader3.bin@0x97470)", mc2vr::shaders::PgDiffSpecMetalFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalFP_pl_sl_li_modern},
    {0xbc86b5e1u, "PgDiffSpecMetalFP_sl (shader3.bin@0x99170)", mc2vr::shaders::PgDiffSpecMetalFP_sl, mc2vr::shaders::PgDiffSpecMetalFP_sl_modern},
    {0x15735729u, "PgDiffSpecMetalFP_sl_li (shader3.bin@0x9ad30)", mc2vr::shaders::PgDiffSpecMetalFP_sl_li, mc2vr::shaders::PgDiffSpecMetalFP_sl_li_modern},
    {0x893ad584u, "PgDiffSpecMetalNormAmbOccFP (shader3.bin@0x9c230)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_modern},
    {0xc7909662u, "PgDiffSpecMetalNormAmbOccFP_pl (shader3.bin@0x9cfb0)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_modern},
    {0xbeabcac2u, "PgDiffSpecMetalNormAmbOccFP_pl_li (shader3.bin@0x9f030)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_li_modern},
    {0x17526fdcu, "PgDiffSpecMetalNormAmbOccFP_pl_sl (shader3.bin@0xa0580)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_sl_modern},
    {0xf0de16c8u, "PgDiffSpecMetalNormAmbOccFP_pl_sl_li (shader3.bin@0xa3650)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_pl_sl_li_modern},
    {0x84ef5980u, "PgDiffSpecMetalNormAmbOccFP_sl (shader3.bin@0xa5510)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_sl, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_sl_modern},
    {0xda008551u, "PgDiffSpecMetalNormAmbOccFP_sl_li (shader3.bin@0xa7290)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecMetalNormAmbOccFP_sl_li_modern},
    {0xca2d6877u, "PgDiffSpecMetalNormAmbOccRimFP (shader3.bin@0xa8950)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_modern},
    {0xcaf7a44eu, "PgDiffSpecMetalNormAmbOccRimFP_pl (shader3.bin@0xa9710)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_modern},
    {0x6504b5a0u, "PgDiffSpecMetalNormAmbOccRimFP_pl_li (shader3.bin@0xab7f0)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_li_modern},
    {0x7813cf43u, "PgDiffSpecMetalNormAmbOccRimFP_pl_sl (shader3.bin@0xacd90)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_sl_modern},
    {0xb80b476cu, "PgDiffSpecMetalNormAmbOccRimFP_pl_sl_li (shader3.bin@0xafeb0)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_pl_sl_li_modern},
    {0x95617cb9u, "PgDiffSpecMetalNormAmbOccRimFP_sl (shader3.bin@0xb1db0)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_sl_modern},
    {0x752ee9cbu, "PgDiffSpecMetalNormAmbOccRimFP_sl_li (shader3.bin@0xb3b70)", mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecMetalNormAmbOccRimFP_sl_li_modern},
    {0x7e9764e1u, "PgDiffSpecMetalNormRimFP (shader3.bin@0xc1870)", mc2vr::shaders::PgDiffSpecMetalNormRimFP, mc2vr::shaders::PgDiffSpecMetalNormRimFP_modern},
    {0x1e6dd9c6u, "PgDiffSpecMetalNormRimFP_pl (shader3.bin@0xc2600)", mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl, mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_modern},
    {0x50a47124u, "PgDiffSpecMetalNormRimFP_pl_li (shader3.bin@0xc46b0)", mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_li, mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_li_modern},
    {0x6be87e07u, "PgDiffSpecMetalNormRimFP_pl_sl (shader3.bin@0xc5c20)", mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_sl_modern},
    {0x2187dbacu, "PgDiffSpecMetalNormRimFP_pl_sl_li (shader3.bin@0xc8d10)", mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalNormRimFP_pl_sl_li_modern},
    {0x70c43ac5u, "PgDiffSpecMetalNormRimFP_sl (shader3.bin@0xcabe0)", mc2vr::shaders::PgDiffSpecMetalNormRimFP_sl, mc2vr::shaders::PgDiffSpecMetalNormRimFP_sl_modern},
    {0x8d29090bu, "PgDiffSpecMetalNormRimFP_sl_li (shader3.bin@0xcc970)", mc2vr::shaders::PgDiffSpecMetalNormRimFP_sl_li, mc2vr::shaders::PgDiffSpecMetalNormRimFP_sl_li_modern},
    {0x1d9cd60fu, "PgDiffSpecMetalRimFP (shader3.bin@0xce040)", mc2vr::shaders::PgDiffSpecMetalRimFP, mc2vr::shaders::PgDiffSpecMetalRimFP_modern},
    {0xc908d975u, "PgDiffSpecMetalRimFP_pl (shader3.bin@0xcec30)", mc2vr::shaders::PgDiffSpecMetalRimFP_pl, mc2vr::shaders::PgDiffSpecMetalRimFP_pl_modern},
    {0x63fd0711u, "PgDiffSpecMetalRimFP_pl_li (shader3.bin@0xd0b20)", mc2vr::shaders::PgDiffSpecMetalRimFP_pl_li, mc2vr::shaders::PgDiffSpecMetalRimFP_pl_li_modern},
    {0xc4f31dc3u, "PgDiffSpecMetalRimFP_pl_sl (shader3.bin@0xd1ed0)", mc2vr::shaders::PgDiffSpecMetalRimFP_pl_sl, mc2vr::shaders::PgDiffSpecMetalRimFP_pl_sl_modern},
    {0xa5700a47u, "PgDiffSpecMetalRimFP_pl_sl_li (shader3.bin@0xd4e10)", mc2vr::shaders::PgDiffSpecMetalRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecMetalRimFP_pl_sl_li_modern},
    {0xf8acead7u, "PgDiffSpecMetalRimFP_sl (shader3.bin@0xd6b40)", mc2vr::shaders::PgDiffSpecMetalRimFP_sl, mc2vr::shaders::PgDiffSpecMetalRimFP_sl_modern},
    {0x0b28ba0au, "PgDiffSpecMetalRimFP_sl_li (shader3.bin@0xd8740)", mc2vr::shaders::PgDiffSpecMetalRimFP_sl_li, mc2vr::shaders::PgDiffSpecMetalRimFP_sl_li_modern},
    {0x3c2a9b93u, "PgDiffSpecNormAmbOccRimFP (shader3.bin@0xe5b00)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_modern},
    {0x3d7d755fu, "PgDiffSpecNormAmbOccRimFP_pl (shader3.bin@0xe67a0)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_modern},
    {0x313e36c3u, "PgDiffSpecNormAmbOccRimFP_pl_li (shader3.bin@0xe8740)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_li_modern},
    {0xd76fd5fbu, "PgDiffSpecNormAmbOccRimFP_pl_sl (shader3.bin@0xe9ba0)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_sl_modern},
    {0x93b3c532u, "PgDiffSpecNormAmbOccRimFP_pl_sl_li (shader3.bin@0xecb90)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_pl_sl_li_modern},
    {0x62f5f029u, "PgDiffSpecNormAmbOccRimFP_sl (shader3.bin@0xee970)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_sl_modern},
    {0x5d4d42fcu, "PgDiffSpecNormAmbOccRimFP_sl_li (shader3.bin@0xf0610)", mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecNormAmbOccRimFP_sl_li_modern},
    {0xb591e0edu, "PgDiffSpecNormRimFP (shader3.bin@0xfd950)", mc2vr::shaders::PgDiffSpecNormRimFP, mc2vr::shaders::PgDiffSpecNormRimFP_modern},
    {0x90e7a94bu, "PgDiffSpecNormRimFP_pl (shader3.bin@0xfe5d0)", mc2vr::shaders::PgDiffSpecNormRimFP_pl, mc2vr::shaders::PgDiffSpecNormRimFP_pl_modern},
    {0x369c6a2bu, "PgDiffSpecNormRimFP_pl_li (shader3.bin@0x100550)", mc2vr::shaders::PgDiffSpecNormRimFP_pl_li, mc2vr::shaders::PgDiffSpecNormRimFP_pl_li_modern},
    {0xcde5b1d7u, "PgDiffSpecNormRimFP_pl_sl (shader3.bin@0x101990)", mc2vr::shaders::PgDiffSpecNormRimFP_pl_sl, mc2vr::shaders::PgDiffSpecNormRimFP_pl_sl_modern},
    {0x62e51096u, "PgDiffSpecNormRimFP_pl_sl_li (shader3.bin@0x104950)", mc2vr::shaders::PgDiffSpecNormRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecNormRimFP_pl_sl_li_modern},
    {0x2b0a75e9u, "PgDiffSpecNormRimFP_sl (shader3.bin@0x106700)", mc2vr::shaders::PgDiffSpecNormRimFP_sl, mc2vr::shaders::PgDiffSpecNormRimFP_sl_modern},
    {0xff3d08a0u, "PgDiffSpecNormRimFP_sl_li (shader3.bin@0x108380)", mc2vr::shaders::PgDiffSpecNormRimFP_sl_li, mc2vr::shaders::PgDiffSpecNormRimFP_sl_li_modern},
    {0x6bce6659u, "PgDiffSpecReflAmbOccFP (shader3.bin@0x109930)", mc2vr::shaders::PgDiffSpecReflAmbOccFP, mc2vr::shaders::PgDiffSpecReflAmbOccFP_modern},
    {0x1ac79e54u, "PgDiffSpecReflAmbOccFP_pl (shader3.bin@0x10a490)", mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl, mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_modern},
    {0x9805c050u, "PgDiffSpecReflAmbOccFP_pl_li (shader3.bin@0x10c2f0)", mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_li_modern},
    {0xa8b43661u, "PgDiffSpecReflAmbOccFP_pl_sl (shader3.bin@0x10d610)", mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_sl_modern},
    {0x698c53bfu, "PgDiffSpecReflAmbOccFP_pl_sl_li (shader3.bin@0x1104b0)", mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflAmbOccFP_pl_sl_li_modern},
    {0x255153eau, "PgDiffSpecReflAmbOccFP_sl (shader3.bin@0x112150)", mc2vr::shaders::PgDiffSpecReflAmbOccFP_sl, mc2vr::shaders::PgDiffSpecReflAmbOccFP_sl_modern},
    {0x49771f4cu, "PgDiffSpecReflAmbOccFP_sl_li (shader3.bin@0x113cb0)", mc2vr::shaders::PgDiffSpecReflAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecReflAmbOccFP_sl_li_modern},
    {0xd04da69bu, "PgDiffSpecReflAmbOccRimFP (shader3.bin@0x115150)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_modern},
    {0xc7b55c26u, "PgDiffSpecReflAmbOccRimFP_pl (shader3.bin@0x115d00)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_modern},
    {0x44aaa820u, "PgDiffSpecReflAmbOccRimFP_pl_li (shader3.bin@0x117ba0)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_li_modern},
    {0x290c8748u, "PgDiffSpecReflAmbOccRimFP_pl_sl (shader3.bin@0x118f00)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_sl_modern},
    {0x6f4ae515u, "PgDiffSpecReflAmbOccRimFP_pl_sl_li (shader3.bin@0x11bde0)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_pl_sl_li_modern},
    {0x026cf3d3u, "PgDiffSpecReflAmbOccRimFP_sl (shader3.bin@0x11dac0)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_sl_modern},
    {0xcf0d952fu, "PgDiffSpecReflAmbOccRimFP_sl_li (shader3.bin@0x11f660)", mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecReflAmbOccRimFP_sl_li_modern},
    {0xeab49df1u, "PgDiffSpecReflFP (shader3.bin@0x120b40)", mc2vr::shaders::PgDiffSpecReflFP, mc2vr::shaders::PgDiffSpecReflFP_modern},
    {0x6eedd02eu, "PgDiffSpecReflFP_pl (shader3.bin@0x121670)", mc2vr::shaders::PgDiffSpecReflFP_pl, mc2vr::shaders::PgDiffSpecReflFP_pl_modern},
    {0x3948414eu, "PgDiffSpecReflFP_pl_li (shader3.bin@0x1234a0)", mc2vr::shaders::PgDiffSpecReflFP_pl_li, mc2vr::shaders::PgDiffSpecReflFP_pl_li_modern},
    {0xfc7e223bu, "PgDiffSpecReflFP_pl_sl (shader3.bin@0x1247a0)", mc2vr::shaders::PgDiffSpecReflFP_pl_sl, mc2vr::shaders::PgDiffSpecReflFP_pl_sl_modern},
    {0x7b6b4fb1u, "PgDiffSpecReflFP_pl_sl_li (shader3.bin@0x127620)", mc2vr::shaders::PgDiffSpecReflFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflFP_pl_sl_li_modern},
    {0x2da86388u, "PgDiffSpecReflFP_sl (shader3.bin@0x129290)", mc2vr::shaders::PgDiffSpecReflFP_sl, mc2vr::shaders::PgDiffSpecReflFP_sl_modern},
    {0x3c3fb826u, "PgDiffSpecReflFP_sl_li (shader3.bin@0x12add0)", mc2vr::shaders::PgDiffSpecReflFP_sl_li, mc2vr::shaders::PgDiffSpecReflFP_sl_li_modern},
    {0x53c94813u, "PgDiffSpecReflNormAmbOccFP (shader3.bin@0x12c240)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_modern},
    {0xaec24769u, "PgDiffSpecReflNormAmbOccFP_pl (shader3.bin@0x12cf20)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_modern},
    {0x37295184u, "PgDiffSpecReflNormAmbOccFP_pl_li (shader3.bin@0x12ef20)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_li_modern},
    {0xbfa05081u, "PgDiffSpecReflNormAmbOccFP_pl_sl (shader3.bin@0x130400)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_sl_modern},
    {0x7a5fcdc9u, "PgDiffSpecReflNormAmbOccFP_pl_sl_li (shader3.bin@0x133440)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_pl_sl_li_modern},
    {0x3bc9d0c9u, "PgDiffSpecReflNormAmbOccFP_sl (shader3.bin@0x135280)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_sl, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_sl_modern},
    {0xbdfaa062u, "PgDiffSpecReflNormAmbOccFP_sl_li (shader3.bin@0x136f80)", mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecReflNormAmbOccFP_sl_li_modern},
    {0xd384dfd5u, "PgDiffSpecReflNormAmbOccRimFP (shader3.bin@0x1385c0)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_modern},
    {0x65569165u, "PgDiffSpecReflNormAmbOccRimFP_pl (shader3.bin@0x139300)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_modern},
    {0x788817a1u, "PgDiffSpecReflNormAmbOccRimFP_pl_li (shader3.bin@0x13b340)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_li_modern},
    {0x7314a200u, "PgDiffSpecReflNormAmbOccRimFP_pl_sl (shader3.bin@0x13c840)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_sl_modern},
    {0x410f8e89u, "PgDiffSpecReflNormAmbOccRimFP_pl_sl_li (shader3.bin@0x13f8c0)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_pl_sl_li_modern},
    {0x0ac70bf2u, "PgDiffSpecReflNormAmbOccRimFP_sl (shader3.bin@0x141740)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_sl_modern},
    {0xdf7fed07u, "PgDiffSpecReflNormAmbOccRimFP_sl_li (shader3.bin@0x143480)", mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecReflNormAmbOccRimFP_sl_li_modern},
    {0x9ed68651u, "PgDiffSpecReflNormFP (shader3.bin@0x144af0)", mc2vr::shaders::PgDiffSpecReflNormFP, mc2vr::shaders::PgDiffSpecReflNormFP_modern},
    {0xd5e9b76du, "PgDiffSpecReflNormFP_pl (shader3.bin@0x1457b0)", mc2vr::shaders::PgDiffSpecReflNormFP_pl, mc2vr::shaders::PgDiffSpecReflNormFP_pl_modern},
    {0xc6469e00u, "PgDiffSpecReflNormFP_pl_li (shader3.bin@0x147780)", mc2vr::shaders::PgDiffSpecReflNormFP_pl_li, mc2vr::shaders::PgDiffSpecReflNormFP_pl_li_modern},
    {0xca69db59u, "PgDiffSpecReflNormFP_pl_sl (shader3.bin@0x148c30)", mc2vr::shaders::PgDiffSpecReflNormFP_pl_sl, mc2vr::shaders::PgDiffSpecReflNormFP_pl_sl_modern},
    {0xe1d4a5c1u, "PgDiffSpecReflNormFP_pl_sl_li (shader3.bin@0x14bc50)", mc2vr::shaders::PgDiffSpecReflNormFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflNormFP_pl_sl_li_modern},
    {0x1062d4adu, "PgDiffSpecReflNormFP_sl (shader3.bin@0x14da60)", mc2vr::shaders::PgDiffSpecReflNormFP_sl, mc2vr::shaders::PgDiffSpecReflNormFP_sl_modern},
    {0x0d212b5au, "PgDiffSpecReflNormFP_sl_li (shader3.bin@0x14f730)", mc2vr::shaders::PgDiffSpecReflNormFP_sl_li, mc2vr::shaders::PgDiffSpecReflNormFP_sl_li_modern},
    {0xd70f371fu, "PgDiffSpecReflNormRimFP (shader3.bin@0x150d40)", mc2vr::shaders::PgDiffSpecReflNormRimFP, mc2vr::shaders::PgDiffSpecReflNormRimFP_modern},
    {0xe8fb00d5u, "PgDiffSpecReflNormRimFP_pl (shader3.bin@0x151a50)", mc2vr::shaders::PgDiffSpecReflNormRimFP_pl, mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_modern},
    {0xf1e09445u, "PgDiffSpecReflNormRimFP_pl_li (shader3.bin@0x153a60)", mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_li, mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_li_modern},
    {0xe225f914u, "PgDiffSpecReflNormRimFP_pl_sl (shader3.bin@0x154f30)", mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_sl, mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_sl_modern},
    {0xa8d8ae95u, "PgDiffSpecReflNormRimFP_pl_sl_li (shader3.bin@0x157f80)", mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflNormRimFP_pl_sl_li_modern},
    {0x72689ffau, "PgDiffSpecReflNormRimFP_sl (shader3.bin@0x159dd0)", mc2vr::shaders::PgDiffSpecReflNormRimFP_sl, mc2vr::shaders::PgDiffSpecReflNormRimFP_sl_modern},
    {0xd0de340bu, "PgDiffSpecReflNormRimFP_sl_li (shader3.bin@0x15bae0)", mc2vr::shaders::PgDiffSpecReflNormRimFP_sl_li, mc2vr::shaders::PgDiffSpecReflNormRimFP_sl_li_modern},
    {0x0f7cd563u, "PgDiffSpecReflRimFP (shader3.bin@0x15d130)", mc2vr::shaders::PgDiffSpecReflRimFP, mc2vr::shaders::PgDiffSpecReflRimFP_modern},
    {0x33ae4020u, "PgDiffSpecReflRimFP_pl (shader3.bin@0x15dcc0)", mc2vr::shaders::PgDiffSpecReflRimFP_pl, mc2vr::shaders::PgDiffSpecReflRimFP_pl_modern},
    {0x2eec16a2u, "PgDiffSpecReflRimFP_pl_li (shader3.bin@0x15fb30)", mc2vr::shaders::PgDiffSpecReflRimFP_pl_li, mc2vr::shaders::PgDiffSpecReflRimFP_pl_li_modern},
    {0xb98bb3beu, "PgDiffSpecReflRimFP_pl_sl (shader3.bin@0x160e60)", mc2vr::shaders::PgDiffSpecReflRimFP_pl_sl, mc2vr::shaders::PgDiffSpecReflRimFP_pl_sl_modern},
    {0x915f2e07u, "PgDiffSpecReflRimFP_pl_sl_li (shader3.bin@0x163d10)", mc2vr::shaders::PgDiffSpecReflRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecReflRimFP_pl_sl_li_modern},
    {0x89c9ec15u, "PgDiffSpecReflRimFP_sl (shader3.bin@0x1659c0)", mc2vr::shaders::PgDiffSpecReflRimFP_sl, mc2vr::shaders::PgDiffSpecReflRimFP_sl_modern},
    {0x2d1a0269u, "PgDiffSpecReflRimFP_sl_li (shader3.bin@0x167530)", mc2vr::shaders::PgDiffSpecReflRimFP_sl_li, mc2vr::shaders::PgDiffSpecReflRimFP_sl_li_modern},
    {0x648606f2u, "PgDiffSpecRimFP (shader3.bin@0x1689e0)", mc2vr::shaders::PgDiffSpecRimFP, mc2vr::shaders::PgDiffSpecRimFP_modern},
    {0x7a724116u, "PgDiffSpecRimFP_pl (shader3.bin@0x1694d0)", mc2vr::shaders::PgDiffSpecRimFP_pl, mc2vr::shaders::PgDiffSpecRimFP_pl_modern},
    {0x0c385244u, "PgDiffSpecRimFP_pl_li (shader3.bin@0x16b2b0)", mc2vr::shaders::PgDiffSpecRimFP_pl_li, mc2vr::shaders::PgDiffSpecRimFP_pl_li_modern},
    {0x72423264u, "PgDiffSpecRimFP_pl_sl (shader3.bin@0x16c550)", mc2vr::shaders::PgDiffSpecRimFP_pl_sl, mc2vr::shaders::PgDiffSpecRimFP_pl_sl_modern},
    {0x41490499u, "PgDiffSpecRimFP_pl_sl_li (shader3.bin@0x16f370)", mc2vr::shaders::PgDiffSpecRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecRimFP_pl_sl_li_modern},
    {0xd0effd34u, "PgDiffSpecRimFP_sl (shader3.bin@0x170f80)", mc2vr::shaders::PgDiffSpecRimFP_sl, mc2vr::shaders::PgDiffSpecRimFP_sl_modern},
    {0x918cb460u, "PgDiffSpecRimFP_sl_li (shader3.bin@0x172a60)", mc2vr::shaders::PgDiffSpecRimFP_sl_li, mc2vr::shaders::PgDiffSpecRimFP_sl_li_modern},
    {0xb07e0034u, "PgDiffSpecSSSAmbOccFP (shader3.bin@0x173e70)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_modern},
    {0x0ee253fau, "PgDiffSpecSSSAmbOccFP_pl (shader3.bin@0x1749e0)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_modern},
    {0xd0296e03u, "PgDiffSpecSSSAmbOccFP_pl_li (shader3.bin@0x176850)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_li_modern},
    {0x3395aebau, "PgDiffSpecSSSAmbOccFP_pl_sl (shader3.bin@0x177b80)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_sl_modern},
    {0x52c8360cu, "PgDiffSpecSSSAmbOccFP_pl_sl_li (shader3.bin@0x17aa30)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_pl_sl_li_modern},
    {0x068fd67cu, "PgDiffSpecSSSAmbOccFP_sl (shader3.bin@0x17c6d0)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP_sl, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_sl_modern},
    {0x71802407u, "PgDiffSpecSSSAmbOccFP_sl_li (shader3.bin@0x17e240)", mc2vr::shaders::PgDiffSpecSSSAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecSSSAmbOccFP_sl_li_modern},
    {0xb842b0d0u, "PgDiffSpecSSSAmbOccRimFP (shader3.bin@0x17f6e0)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_modern},
    {0x29fad383u, "PgDiffSpecSSSAmbOccRimFP_pl (shader3.bin@0x1803a0)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_modern},
    {0x11731f0du, "PgDiffSpecSSSAmbOccRimFP_pl_li (shader3.bin@0x182360)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_li_modern},
    {0xa5e0b1e7u, "PgDiffSpecSSSAmbOccRimFP_pl_sl (shader3.bin@0x1837e0)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_sl_modern},
    {0x08052d7du, "PgDiffSpecSSSAmbOccRimFP_pl_sl_li (shader3.bin@0x1867e0)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_pl_sl_li_modern},
    {0x1c7c4e26u, "PgDiffSpecSSSAmbOccRimFP_sl (shader3.bin@0x1885d0)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_sl_modern},
    {0x66fee425u, "PgDiffSpecSSSAmbOccRimFP_sl_li (shader3.bin@0x18a290)", mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecSSSAmbOccRimFP_sl_li_modern},
    {0xcd6db784u, "PgDiffSpecSSSFP (shader3.bin@0x18b880)", mc2vr::shaders::PgDiffSpecSSSFP, mc2vr::shaders::PgDiffSpecSSSFP_modern},
    {0x417f0a58u, "PgDiffSpecSSSFP_pl (shader3.bin@0x18c3c0)", mc2vr::shaders::PgDiffSpecSSSFP_pl, mc2vr::shaders::PgDiffSpecSSSFP_pl_modern},
    {0x3eb3df1du, "PgDiffSpecSSSFP_pl_li (shader3.bin@0x18e200)", mc2vr::shaders::PgDiffSpecSSSFP_pl_li, mc2vr::shaders::PgDiffSpecSSSFP_pl_li_modern},
    {0x54bd3628u, "PgDiffSpecSSSFP_pl_sl (shader3.bin@0x18f500)", mc2vr::shaders::PgDiffSpecSSSFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSFP_pl_sl_modern},
    {0x5805020au, "PgDiffSpecSSSFP_pl_sl_li (shader3.bin@0x192380)", mc2vr::shaders::PgDiffSpecSSSFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSFP_pl_sl_li_modern},
    {0x84160be2u, "PgDiffSpecSSSFP_sl (shader3.bin@0x194000)", mc2vr::shaders::PgDiffSpecSSSFP_sl, mc2vr::shaders::PgDiffSpecSSSFP_sl_modern},
    {0x32729e69u, "PgDiffSpecSSSFP_sl_li (shader3.bin@0x195b40)", mc2vr::shaders::PgDiffSpecSSSFP_sl_li, mc2vr::shaders::PgDiffSpecSSSFP_sl_li_modern},
    {0xb1fd78f7u, "PgDiffSpecSSSNormAmbOccFP (shader3.bin@0x196fc0)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_modern},
    {0xafd7191eu, "PgDiffSpecSSSNormAmbOccFP_pl (shader3.bin@0x197cd0)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_modern},
    {0x336ebed6u, "PgDiffSpecSSSNormAmbOccFP_pl_li (shader3.bin@0x199cf0)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_li, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_li_modern},
    {0xc9534399u, "PgDiffSpecSSSNormAmbOccFP_pl_sl (shader3.bin@0x19b1c0)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_sl_modern},
    {0xc9d70835u, "PgDiffSpecSSSNormAmbOccFP_pl_sl_li (shader3.bin@0x19e210)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_pl_sl_li_modern},
    {0x381612a6u, "PgDiffSpecSSSNormAmbOccFP_sl (shader3.bin@0x1a0050)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_sl, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_sl_modern},
    {0xec171612u, "PgDiffSpecSSSNormAmbOccFP_sl_li (shader3.bin@0x1a1d60)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_sl_li, mc2vr::shaders::PgDiffSpecSSSNormAmbOccFP_sl_li_modern},
    {0x9f6dc5b0u, "PgDiffSpecSSSNormAmbOccRimFP (shader3.bin@0x1a33a0)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_modern},
    {0x508c3242u, "PgDiffSpecSSSNormAmbOccRimFP_pl (shader3.bin@0x1a4200)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_modern},
    {0x3e9f81cbu, "PgDiffSpecSSSNormAmbOccRimFP_pl_li (shader3.bin@0x1a6370)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_li, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_li_modern},
    {0x6fc8ec91u, "PgDiffSpecSSSNormAmbOccRimFP_pl_sl (shader3.bin@0x1a7990)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_sl_modern},
    {0xfe32d168u, "PgDiffSpecSSSNormAmbOccRimFP_pl_sl_li (shader3.bin@0x1aab30)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_pl_sl_li_modern},
    {0x4807e43au, "PgDiffSpecSSSNormAmbOccRimFP_sl (shader3.bin@0x1acae0)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_sl, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_sl_modern},
    {0x91592974u, "PgDiffSpecSSSNormAmbOccRimFP_sl_li (shader3.bin@0x1ae940)", mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_sl_li, mc2vr::shaders::PgDiffSpecSSSNormAmbOccRimFP_sl_li_modern},
    {0x1aa70e81u, "PgDiffSpecSSSNormFP (shader3.bin@0x1b00d0)", mc2vr::shaders::PgDiffSpecSSSNormFP, mc2vr::shaders::PgDiffSpecSSSNormFP_modern},
    {0x86e0a0aau, "PgDiffSpecSSSNormFP_pl (shader3.bin@0x1b0db0)", mc2vr::shaders::PgDiffSpecSSSNormFP_pl, mc2vr::shaders::PgDiffSpecSSSNormFP_pl_modern},
    {0xf454d65au, "PgDiffSpecSSSNormFP_pl_li (shader3.bin@0x1b2da0)", mc2vr::shaders::PgDiffSpecSSSNormFP_pl_li, mc2vr::shaders::PgDiffSpecSSSNormFP_pl_li_modern},
    {0x9c76c661u, "PgDiffSpecSSSNormFP_pl_sl (shader3.bin@0x1b4240)", mc2vr::shaders::PgDiffSpecSSSNormFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSNormFP_pl_sl_modern},
    {0x155cb8f5u, "PgDiffSpecSSSNormFP_pl_sl_li (shader3.bin@0x1b7260)", mc2vr::shaders::PgDiffSpecSSSNormFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSNormFP_pl_sl_li_modern},
    {0x3e83526au, "PgDiffSpecSSSNormFP_sl (shader3.bin@0x1b9080)", mc2vr::shaders::PgDiffSpecSSSNormFP_sl, mc2vr::shaders::PgDiffSpecSSSNormFP_sl_modern},
    {0x28f357f2u, "PgDiffSpecSSSNormFP_sl_li (shader3.bin@0x1bad60)", mc2vr::shaders::PgDiffSpecSSSNormFP_sl_li, mc2vr::shaders::PgDiffSpecSSSNormFP_sl_li_modern},
    {0x163b45b2u, "PgDiffSpecSSSNormRimFP (shader3.bin@0x1bc370)", mc2vr::shaders::PgDiffSpecSSSNormRimFP, mc2vr::shaders::PgDiffSpecSSSNormRimFP_modern},
    {0x480cec92u, "PgDiffSpecSSSNormRimFP_pl (shader3.bin@0x1bd1a0)", mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl, mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_modern},
    {0x80c2cb6bu, "PgDiffSpecSSSNormRimFP_pl_li (shader3.bin@0x1bf2e0)", mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_li, mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_li_modern},
    {0x00da4b2du, "PgDiffSpecSSSNormRimFP_pl_sl (shader3.bin@0x1c08d0)", mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_sl_modern},
    {0xe02a8388u, "PgDiffSpecSSSNormRimFP_pl_sl_li (shader3.bin@0x1c3a40)", mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSNormRimFP_pl_sl_li_modern},
    {0xe16eb3c2u, "PgDiffSpecSSSNormRimFP_sl (shader3.bin@0x1c59c0)", mc2vr::shaders::PgDiffSpecSSSNormRimFP_sl, mc2vr::shaders::PgDiffSpecSSSNormRimFP_sl_modern},
    {0xd299fa38u, "PgDiffSpecSSSNormRimFP_sl_li (shader3.bin@0x1c77f0)", mc2vr::shaders::PgDiffSpecSSSNormRimFP_sl_li, mc2vr::shaders::PgDiffSpecSSSNormRimFP_sl_li_modern},
    {0x961fd968u, "PgDiffSpecSSSRimFP (shader3.bin@0x1c8f50)", mc2vr::shaders::PgDiffSpecSSSRimFP, mc2vr::shaders::PgDiffSpecSSSRimFP_modern},
    {0xfca26e79u, "PgDiffSpecSSSRimFP_pl (shader3.bin@0x1c9be0)", mc2vr::shaders::PgDiffSpecSSSRimFP_pl, mc2vr::shaders::PgDiffSpecSSSRimFP_pl_modern},
    {0x60265cebu, "PgDiffSpecSSSRimFP_pl_li (shader3.bin@0x1cbb70)", mc2vr::shaders::PgDiffSpecSSSRimFP_pl_li, mc2vr::shaders::PgDiffSpecSSSRimFP_pl_li_modern},
    {0x5d089905u, "PgDiffSpecSSSRimFP_pl_sl (shader3.bin@0x1ccfc0)", mc2vr::shaders::PgDiffSpecSSSRimFP_pl_sl, mc2vr::shaders::PgDiffSpecSSSRimFP_pl_sl_modern},
    {0x144fffb3u, "PgDiffSpecSSSRimFP_pl_sl_li (shader3.bin@0x1cff90)", mc2vr::shaders::PgDiffSpecSSSRimFP_pl_sl_li, mc2vr::shaders::PgDiffSpecSSSRimFP_pl_sl_li_modern},
    {0x3a116f80u, "PgDiffSpecSSSRimFP_sl (shader3.bin@0x1d1d60)", mc2vr::shaders::PgDiffSpecSSSRimFP_sl, mc2vr::shaders::PgDiffSpecSSSRimFP_sl_modern},
    {0x1398935bu, "PgDiffSpecSSSRimFP_sl_li (shader3.bin@0x1d39f0)", mc2vr::shaders::PgDiffSpecSSSRimFP_sl_li, mc2vr::shaders::PgDiffSpecSSSRimFP_sl_li_modern},
    {0x3e76835cu, "PgFastFP (shader3.bin@0x1d6370)", mc2vr::shaders::PgFastFP, mc2vr::shaders::PgFastFP_modern, kAddsCameraPos},
    {0x1055364cu, "PgFastFP_pl (shader3.bin@0x1d6930)", mc2vr::shaders::PgFastFP_pl, mc2vr::shaders::PgFastFP_pl_modern, kAddsCameraPos},
    {0x19b8861cu, "PgFastFP_pl_li (shader3.bin@0x1d7920)", mc2vr::shaders::PgFastFP_pl_li, mc2vr::shaders::PgFastFP_pl_li_modern, kAddsCameraPos},
    {0x2aa895fbu, "PgFastFP_pl_sl (shader3.bin@0x1d8350)", mc2vr::shaders::PgFastFP_pl_sl, mc2vr::shaders::PgFastFP_pl_sl_modern, kAddsCameraPos},
    {0x7d01c4f5u, "PgFastFP_pl_sl_li (shader3.bin@0x1d9f40)", mc2vr::shaders::PgFastFP_pl_sl_li, mc2vr::shaders::PgFastFP_pl_sl_li_modern, kAddsCameraPos},
    {0x8d347471u, "PgFastFP_sl (shader3.bin@0x1db080)", mc2vr::shaders::PgFastFP_sl, mc2vr::shaders::PgFastFP_sl_modern, kAddsCameraPos},
    {0x5fa41029u, "PgFastFP_sl_li (shader3.bin@0x1dc250)", mc2vr::shaders::PgFastFP_sl_li, mc2vr::shaders::PgFastFP_sl_li_modern, kAddsCameraPos},
    {0x765f3420u, "PgScrubLightFP (shader3.bin@0x2004c0)", mc2vr::shaders::PgScrubLightFP, mc2vr::shaders::PgScrubLightFP_modern, kAddsCameraPos},
    {0x523d88a3u, "PgScrubLightFP_pl (shader3.bin@0x200a80)", mc2vr::shaders::PgScrubLightFP_pl, mc2vr::shaders::PgScrubLightFP_pl_modern, kAddsCameraPos},
    {0x4249bd13u, "PgScrubLightFP_pl_li (shader3.bin@0x201a60)", mc2vr::shaders::PgScrubLightFP_pl_li, mc2vr::shaders::PgScrubLightFP_pl_li_modern, kAddsCameraPos},
    {0x788c2fddu, "PgScrubLightFP_pl_sl (shader3.bin@0x202480)", mc2vr::shaders::PgScrubLightFP_pl_sl, mc2vr::shaders::PgScrubLightFP_pl_sl_modern, kAddsCameraPos},
    {0x59ca705eu, "PgScrubLightFP_pl_sl_li (shader3.bin@0x204060)", mc2vr::shaders::PgScrubLightFP_pl_sl_li, mc2vr::shaders::PgScrubLightFP_pl_sl_li_modern, kAddsCameraPos},
    {0x9de607f6u, "PgScrubLightFP_sl (shader3.bin@0x205180)", mc2vr::shaders::PgScrubLightFP_sl, mc2vr::shaders::PgScrubLightFP_sl_modern, kAddsCameraPos},
    {0x307dcccdu, "PgScrubLightFP_sl_li (shader3.bin@0x206340)", mc2vr::shaders::PgScrubLightFP_sl_li, mc2vr::shaders::PgScrubLightFP_sl_li_modern, kAddsCameraPos},
    {0x972e1b82u, "PgTerrainMeshFP (shader3.bin@0x2439e0)", mc2vr::shaders::PgTerrainMeshFP, mc2vr::shaders::PgTerrainMeshFP_modern, kAddsCameraPos},
    {0xd6862fe2u, "PgTerrainMeshFP_pl (shader3.bin@0x2441e0)", mc2vr::shaders::PgTerrainMeshFP_pl, mc2vr::shaders::PgTerrainMeshFP_pl_modern, kAddsCameraPos},
    {0x4e3ec610u, "PgTerrainMeshFP_pl_li (shader3.bin@0x2453c0)", mc2vr::shaders::PgTerrainMeshFP_pl_li, mc2vr::shaders::PgTerrainMeshFP_pl_li_modern, kAddsCameraPos},
    {0xdb681215u, "PgTerrainMeshFP_pl_sl (shader3.bin@0x245fe0)", mc2vr::shaders::PgTerrainMeshFP_pl_sl, mc2vr::shaders::PgTerrainMeshFP_pl_sl_modern, kAddsCameraPos},
    {0xd8b6ba48u, "PgTerrainMeshFP_pl_sl_li (shader3.bin@0x247dc0)", mc2vr::shaders::PgTerrainMeshFP_pl_sl_li, mc2vr::shaders::PgTerrainMeshFP_pl_sl_li_modern, kAddsCameraPos},
    {0xa37e1546u, "PgTerrainMeshFP_sl (shader3.bin@0x2490f0)", mc2vr::shaders::PgTerrainMeshFP_sl, mc2vr::shaders::PgTerrainMeshFP_sl_modern, kAddsCameraPos},
    {0x9a8facf5u, "PgTerrainMeshFP_sl_li (shader3.bin@0x24a4c0)", mc2vr::shaders::PgTerrainMeshFP_sl_li, mc2vr::shaders::PgTerrainMeshFP_sl_li_modern, kAddsCameraPos},
    {0x71b9f2a4u, "PgTerrainMeshFP1D (shader3.bin@0x222850)", mc2vr::shaders::PgTerrainMeshFP1D, mc2vr::shaders::PgTerrainMeshFP1D_modern, kAddsCameraPos},
    {0x1414c36au, "PgTerrainMeshFP1D_pl (shader3.bin@0x223140)", mc2vr::shaders::PgTerrainMeshFP1D_pl, mc2vr::shaders::PgTerrainMeshFP1D_pl_modern, kAddsCameraPos},
    {0x368fe8b4u, "PgTerrainMeshFP1D_pl_li (shader3.bin@0x224420)", mc2vr::shaders::PgTerrainMeshFP1D_pl_li, mc2vr::shaders::PgTerrainMeshFP1D_pl_li_modern, kAddsCameraPos},
    {0x175fa56fu, "PgTerrainMeshFP1D_pl_sl (shader3.bin@0x225140)", mc2vr::shaders::PgTerrainMeshFP1D_pl_sl, mc2vr::shaders::PgTerrainMeshFP1D_pl_sl_modern, kAddsCameraPos},
    {0xcfc98e76u, "PgTerrainMeshFP1D_pl_sl_li (shader3.bin@0x227020)", mc2vr::shaders::PgTerrainMeshFP1D_pl_sl_li, mc2vr::shaders::PgTerrainMeshFP1D_pl_sl_li_modern, kAddsCameraPos},
    {0xc126500du, "PgTerrainMeshFP1D_sl (shader3.bin@0x228440)", mc2vr::shaders::PgTerrainMeshFP1D_sl, mc2vr::shaders::PgTerrainMeshFP1D_sl_modern, kAddsCameraPos},
    {0xed15509au, "PgTerrainMeshFP1D_sl_li (shader3.bin@0x229900)", mc2vr::shaders::PgTerrainMeshFP1D_sl_li, mc2vr::shaders::PgTerrainMeshFP1D_sl_li_modern, kAddsCameraPos},
    {0x5ad7189au, "PgTerrainMeshFP2D (shader3.bin@0x22a8d0)", mc2vr::shaders::PgTerrainMeshFP2D, mc2vr::shaders::PgTerrainMeshFP2D_modern, kAddsCameraPos},
    {0xbe3f0f57u, "PgTerrainMeshFP2D_pl (shader3.bin@0x22b220)", mc2vr::shaders::PgTerrainMeshFP2D_pl, mc2vr::shaders::PgTerrainMeshFP2D_pl_modern, kAddsCameraPos},
    {0x91612249u, "PgTerrainMeshFP2D_pl_li (shader3.bin@0x22c560)", mc2vr::shaders::PgTerrainMeshFP2D_pl_li, mc2vr::shaders::PgTerrainMeshFP2D_pl_li_modern, kAddsCameraPos},
    {0xc62e1ae0u, "PgTerrainMeshFP2D_pl_sl (shader3.bin@0x22d2d0)", mc2vr::shaders::PgTerrainMeshFP2D_pl_sl, mc2vr::shaders::PgTerrainMeshFP2D_pl_sl_modern, kAddsCameraPos},
    {0xe16ea2cdu, "PgTerrainMeshFP2D_pl_sl_li (shader3.bin@0x22f210)", mc2vr::shaders::PgTerrainMeshFP2D_pl_sl_li, mc2vr::shaders::PgTerrainMeshFP2D_pl_sl_li_modern, kAddsCameraPos},
    {0xcd9c5360u, "PgTerrainMeshFP2D_sl (shader3.bin@0x230690)", mc2vr::shaders::PgTerrainMeshFP2D_sl, mc2vr::shaders::PgTerrainMeshFP2D_sl_modern, kAddsCameraPos},
    {0xc27ddedfu, "PgTerrainMeshFP2D_sl_li (shader3.bin@0x231bb0)", mc2vr::shaders::PgTerrainMeshFP2D_sl_li, mc2vr::shaders::PgTerrainMeshFP2D_sl_li_modern, kAddsCameraPos},
    {0x3af611d5u, "PgTerrainMeshFP3D (shader3.bin@0x232be0)", mc2vr::shaders::PgTerrainMeshFP3D, mc2vr::shaders::PgTerrainMeshFP3D_modern, kAddsCameraPos},
    {0x5cfcf8cdu, "PgTerrainMeshFP3D_pl (shader3.bin@0x233590)", mc2vr::shaders::PgTerrainMeshFP3D_pl, mc2vr::shaders::PgTerrainMeshFP3D_pl_modern, kAddsCameraPos},
    {0xaee7ce7fu, "PgTerrainMeshFP3D_pl_li (shader3.bin@0x234930)", mc2vr::shaders::PgTerrainMeshFP3D_pl_li, mc2vr::shaders::PgTerrainMeshFP3D_pl_li_modern, kAddsCameraPos},
    {0xbe3ac53bu, "PgTerrainMeshFP3D_pl_sl (shader3.bin@0x235700)", mc2vr::shaders::PgTerrainMeshFP3D_pl_sl, mc2vr::shaders::PgTerrainMeshFP3D_pl_sl_modern, kAddsCameraPos},
    {0x1bda072au, "PgTerrainMeshFP3D_pl_sl_li (shader3.bin@0x2376a0)", mc2vr::shaders::PgTerrainMeshFP3D_pl_sl_li, mc2vr::shaders::PgTerrainMeshFP3D_pl_sl_li_modern, kAddsCameraPos},
    {0x5aafbdb2u, "PgTerrainMeshFP3D_sl (shader3.bin@0x238b80)", mc2vr::shaders::PgTerrainMeshFP3D_sl, mc2vr::shaders::PgTerrainMeshFP3D_sl_modern, kAddsCameraPos},
    {0xf763b721u, "PgTerrainMeshFP3D_sl_li (shader3.bin@0x23a100)", mc2vr::shaders::PgTerrainMeshFP3D_sl_li, mc2vr::shaders::PgTerrainMeshFP3D_sl_li_modern, kAddsCameraPos},
    {0xb87cace9u, "PgTerrainMeshFP4D (shader3.bin@0x23b190)", mc2vr::shaders::PgTerrainMeshFP4D, mc2vr::shaders::PgTerrainMeshFP4D_modern, kAddsCameraPos},
    {0x32255d76u, "PgTerrainMeshFP4D_pl (shader3.bin@0x23bba0)", mc2vr::shaders::PgTerrainMeshFP4D_pl, mc2vr::shaders::PgTerrainMeshFP4D_pl_modern, kAddsCameraPos},
    {0x0a525994u, "PgTerrainMeshFP4D_pl_li (shader3.bin@0x23cfa0)", mc2vr::shaders::PgTerrainMeshFP4D_pl_li, mc2vr::shaders::PgTerrainMeshFP4D_pl_li_modern, kAddsCameraPos},
    {0x7c08c23cu, "PgTerrainMeshFP4D_pl_sl (shader3.bin@0x23ddd0)", mc2vr::shaders::PgTerrainMeshFP4D_pl_sl, mc2vr::shaders::PgTerrainMeshFP4D_pl_sl_modern, kAddsCameraPos},
    {0x137b06a9u, "PgTerrainMeshFP4D_pl_sl_li (shader3.bin@0x23fdd0)", mc2vr::shaders::PgTerrainMeshFP4D_pl_sl_li, mc2vr::shaders::PgTerrainMeshFP4D_pl_sl_li_modern, kAddsCameraPos},
    {0x17d2ce31u, "PgTerrainMeshFP4D_sl (shader3.bin@0x241310)", mc2vr::shaders::PgTerrainMeshFP4D_sl, mc2vr::shaders::PgTerrainMeshFP4D_sl_modern, kAddsCameraPos},
    {0x304c3df6u, "PgTerrainMeshFP4D_sl_li (shader3.bin@0x2428f0)", mc2vr::shaders::PgTerrainMeshFP4D_sl_li, mc2vr::shaders::PgTerrainMeshFP4D_sl_li_modern, kAddsCameraPos},
    {0xedb3eaeeu, "PgUnderwaterFP (shader3.bin@0x24f4d0)", mc2vr::shaders::PgUnderwaterFP, nullptr},
    {0xc2dde179u, "PgWaterFPDWE (shader3.bin@0x254a80)", mc2vr::shaders::PgWaterFPDWE, nullptr},
    {0x06f59bd8u, "PgWaterFP (shader3.bin@0x2571d0)", mc2vr::shaders::PgWaterFP, nullptr},
    {0x7050482eu, "PgWaterFP_LI (shader3.bin@0x25c3d0)", mc2vr::shaders::PgWaterFP_LI, nullptr},
    {0xdcf34dbdu, "PgWaterFPbl (shader3.bin@0x24fe80)", mc2vr::shaders::PgWaterFPbl, nullptr},
};

uint32_t g_seen = 0, g_reported_seen = 0;  // CreatePixelShader calls
bool g_modern = true;                       // shading=modern (the default)

// The entry's bytecode in the current shading.
const uint32_t *chosen(const Entry &e)
{
    return (g_modern && e.modern != nullptr) ? e.modern : e.code;
}

// The bytecode's length in bytes: tokens up to and including the end token
// (comments skipped by their length), 0 if it doesn't look like SM2/3.
uint32_t bytecode_size(const uint32_t *t)
{
    if ((t[0] & 0xffff0000u) != 0xffff0000u) return 0;
    constexpr uint32_t kMaxWords = 1u << 18;
    uint32_t i = 1;
    while (i < kMaxWords) {
        const uint32_t op = t[i] & 0xffff;
        if (t[i] == 0x0000ffffu) return (i + 1) * 4;
        if (op == 0xfffe) {
            i += 1 + ((t[i] >> 16) & 0x7fff);
        } else {
            i += 1 + ((t[i] >> 24) & 0xf);
        }
    }
    return 0;
}

uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

} // namespace

namespace {

// ---- shadow_resolution: the 9-tap receivers' atlas texel constants ----------------
//
// Every shadow receiver with the 9-tap kernel (the game's 315 and ours) holds
// the atlas texel size in `def` constants: +-0.5/1024, +-1.5/1024, +-0.5/4096,
// +-1.5/4096 (tap offsets) and 1024, 4096 (the bilinear weights' scale), and
// no other shader in shader3.bin uses those values (docs/shadows.md). At a
// cascade edge of k * 1024 the offsets scale by 1/k and the sizes by k (exact:
// powers of two). Matched by exact float bits; a shader qualifies only with
// both half-texel signatures.
std::vector<std::vector<uint32_t> *> g_scaled;  // kept for the device's lifetime (leaked by design)
uint32_t g_scaled_count = 0;

uint32_t f32_bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// `code` with its texel constants scaled, or `code` if none qualify.
const uint32_t *texel_scaled(const uint32_t *code)
{
    const uint32_t cascade = reimpl::shadow_cascade_size();
    if (cascade == 1024 || code == nullptr) return code;
    const uint32_t size = bytecode_size(code);
    if (size == 0) return code;
    const float k = (float)cascade / 1024.0f;
    const float offsets[] = {0.5f / 1024, 1.5f / 1024, 0.5f / 4096, 1.5f / 4096, 1.0f / 1024, 1.0f / 4096};
    const float sizes[] = {1024.0f, 4096.0f};
    auto *out = new std::vector<uint32_t>(code, code + size / 4);
    uint32_t *t = out->data();
    bool sig1024 = false, sig4096 = false;
    int changed = 0;
    for (int pass = 0; pass < 2; pass++) {  // 0: find the signatures; 1: scale
        for (uint32_t i = 1; i < size / 4;) {
            const uint32_t op = t[i] & 0xffff;
            if (t[i] == 0x0000ffffu) break;
            if (op == 0xfffe) {
                i += 1 + ((t[i] >> 16) & 0x7fff);
                continue;
            }
            const uint32_t len = (t[i] >> 24) & 0xf;
            if (op == 81 && len == 5) {  // def c#, x, y, z, w
                for (uint32_t c = 0; c < 4; c++) {
                    uint32_t &v = t[i + 2 + c];
                    const uint32_t mag = v & 0x7fffffffu;
                    if (pass == 0) {
                        sig1024 |= mag == f32_bits(0.5f / 1024);
                        sig4096 |= mag == f32_bits(0.5f / 4096);
                        continue;
                    }
                    for (float o : offsets) {
                        if (mag == f32_bits(o)) {
                            v = (v & 0x80000000u) | f32_bits(o / k);
                            changed++;
                        }
                    }
                    for (float s : sizes) {
                        if (v == f32_bits(s)) {
                            v = f32_bits(s * k);
                            changed++;
                        }
                    }
                }
            }
            i += 1 + len;
        }
        if (pass == 0 && !(sig1024 && sig4096)) break;
    }
    if (changed == 0) {
        delete out;
        return code;
    }
    g_scaled.push_back(out);
    g_scaled_count++;
    if (g_scaled_count == 1) {
        MC2VR_LOG("shaders: shadow_resolution=%u: scaling the 9-tap receivers' atlas texel constants "
                  "(first: %d values; count in the window line)", cascade, changed);
    }
    return out->data();
}

} // namespace

bool set_shading(const char *value)
{
    if (std::strcmp(value, "classic") == 0) g_modern = false;
    else if (std::strcmp(value, "modern") == 0) g_modern = true;
    else return false;
    MC2VR_LOG("shaders: shading=%s (%s)", g_modern ? "modern" : "classic",
              g_modern ? "the physically based _modern shaders (all but the water)" : "the originals' shading");
    return true;
}

const uint32_t *substitute_pixel_shader(const uint32_t *original)
{
    g_seen++;
    if (original == nullptr) return original;
    const uint32_t size = bytecode_size(original);
    if (size == 0) return original;
    const uint32_t hash = fnv1a(reinterpret_cast<const uint8_t *>(original), size);
    for (Entry &e : g_entries) {
        if (e.original != hash || e.failed) continue;
        e.created++;
        if (e.created == 1) {
            MC2VR_LOG("shaders: %08x %s -> reimplementation%s (%u bytes; later creations counted in the window "
                      "line)",
                      hash, e.name, chosen(e) == e.modern ? ", modern" : "", bytecode_size(chosen(e)));
        }
        e.last_modern = chosen(e) == e.modern;
        e.last_used = texel_scaled(chosen(e));
        return e.last_used;
    }
    return texel_scaled(original);
}

const uint32_t *on_substitute_failed(const uint32_t *original, const uint32_t *used, long hr)
{
    for (Entry &e : g_entries) {
        if (e.last_used != used || used == nullptr) continue;
        e.failed = true;
        MC2VR_LOG("shaders: ERROR — the device refused the reimplementation of %08x %s (hr=0x%08lx); "
                  "the original is used",
                  e.original, e.name, (unsigned long)hr);
    }
    return texel_scaled(original);
}

namespace {
struct Created {
    const void *object;
    const char *name;
};
std::vector<Created> g_created;  // load thread writes at creation; debug readers on the render thread

} // namespace

void on_created(const uint32_t *used, const void *object)
{
    if (object == nullptr || used == nullptr) return;
    for (const Entry &e : g_entries) {
        if (e.last_used == used) {
            g_created.push_back(Created{object, e.name});
            return;
        }
    }
}

const char *name_of(const void *object)
{
    for (const Created &c : g_created) {
        if (c.object == object) return c.name;
    }
    return nullptr;
}

void report_window()
{
    // Oracle: each reimplemented shader is substituted once per creation by
    // the game (on load); created=0 for a shader the game never created.
    if (g_seen == g_reported_seen) return;
    g_reported_seen = g_seen;
    char line[1024];  // 27+ entries of " xxxxxxxx=n"
    int n = 0;
    for (const Entry &e : g_entries) {
        n += std::snprintf(line + n, sizeof line - (size_t)n, " %08x=%u%s", e.original, e.created,
                           e.failed ? "(failed)" : "");
        if (n >= (int)sizeof line) break;
    }
    MC2VR_LOG("shaders: window: shading=%s CreatePixelShader calls=%u texelScaled=%u | substituted:%s",
              g_modern ? "modern" : "classic", g_seen, g_scaled_count, line);
}

} // namespace mc2vr::hooks::shader_reimpl
