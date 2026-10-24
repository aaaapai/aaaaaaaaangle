//
// Copyright 2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// mathutil.cpp: Math and bit manipulation functions.

#include "common/mathutil.h"

#include <math.h>
#include <algorithm>

namespace gl
{

namespace
{

struct RGB9E5Data
{
    unsigned int R : 9;
    unsigned int G : 9;
    unsigned int B : 9;
    unsigned int E : 5;
};

// B is the exponent bias (15)
constexpr int g_sharedexp_bias = 15;

// N is the number of mantissa bits per component (9)
constexpr int g_sharedexp_mantissabits = 9;

// number of mantissa bits per component pre-biased
constexpr int g_sharedexp_biased_mantissabits = g_sharedexp_bias + g_sharedexp_mantissabits;

// Emax is the maximum allowed biased exponent value (31)
constexpr int g_sharedexp_maxexponent = 31;

constexpr float g_sharedexp_max =
    ((static_cast<float>(1 << g_sharedexp_mantissabits) - 1) /
     static_cast<float>(1 << g_sharedexp_mantissabits)) *
    static_cast<float>(1 << (g_sharedexp_maxexponent - g_sharedexp_bias));

}  // anonymous namespace

unsigned int convertRGBFloatsTo999E5(float red, float green, float blue)
{
    const float red_c   = std::max<float>(0, std::min(g_sharedexp_max, red));
    const float green_c = std::max<float>(0, std::min(g_sharedexp_max, green));
    const float blue_c  = std::max<float>(0, std::min(g_sharedexp_max, blue));

    const float max_c = std::max<float>({red_c, green_c, blue_c});
    // Use log2 and exp2 for faster and more accurate exponent calculation
    const float exp_p = std::max<float>(-g_sharedexp_bias - 1,
                                        std::floor(std::log2(max_c))) + 1 + g_sharedexp_bias;
    const int max_s = static_cast<int>(
        std::floor((max_c / std::exp2(exp_p - g_sharedexp_biased_mantissabits)) + 0.5f));
    const int exp_s = static_cast<int>((max_s < (1 << g_sharedexp_mantissabits)) ? exp_p : exp_p + 1);
    const float pow2_exp = std::exp2(static_cast<float>(exp_s) - g_sharedexp_biased_mantissabits);

    RGB9E5Data output;
    output.R = static_cast<unsigned int>(std::floor((red_c / pow2_exp) + 0.5f));
    output.G = static_cast<unsigned int>(std::floor((green_c / pow2_exp) + 0.5f));
    output.B = static_cast<unsigned int>(std::floor((blue_c / pow2_exp) + 0.5f));
    output.E = exp_s;

    return bitCast<unsigned int>(output);
}

void convert999E5toRGBFloats(unsigned int input, float *red, float *green, float *blue)
{
    const RGB9E5Data *inputData = reinterpret_cast<const RGB9E5Data *>(&input);

    // Use ldexp for faster exponent to float conversion
    const float pow2_exp = std::ldexp(1.0f, inputData->E - g_sharedexp_biased_mantissabits);

    *red   = inputData->R * pow2_exp;
    *green = inputData->G * pow2_exp;
    *blue  = inputData->B * pow2_exp;
}

std::ostream &operator<<(std::ostream &s, const IndexRange &a)
{
    if (a.isEmpty())
    {
        s << "[]";
    }
    else
    {
        s << "[" << a.start() << ", " << a.end() << "]";
    }
    return s;
}

}  // namespace gl
