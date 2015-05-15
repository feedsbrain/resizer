/*
 * Copyright (c) Imazen LLC.
 * No part of this project, including this file, may be copied, modified,
 * propagated, or distributed except as permitted in COPYRIGHT.txt.
 * Licensed under the GNU Affero General Public License, Version 3.0.
 * Commercial licenses available at http://imageresizing.net/
 */
#ifdef _MSC_VER
#pragma unmanaged
#endif

#include "fastscaling_private.h"

#include <string.h>

#ifndef _MSC_VER
#include <alloca.h>
#else
#pragma unmanaged
#ifndef alloca
#include <malloc.h>
#define alloca _alloca
#endif
#endif

ConvolutionKernel * ConvolutionKernel_create(Context * context, uint32_t radius)
{
    ConvolutionKernel * k = CONTEXT_calloc_array(context, 1, ConvolutionKernel);
    //For the actual array;
    float * a = CONTEXT_calloc_array(context,radius * 2 + 1, float);
    //we assume a maximum of 4 channels are going to need buffering during convolution
    float * buf = (float *)CONTEXT_malloc(context, (radius +2) * 4 * sizeof(float));

    if (k == NULL || a == NULL || buf == NULL) {
        CONTEXT_free(context, k);
        CONTEXT_free(context, a);
        CONTEXT_free(context, buf);
        CONTEXT_error(context, Out_of_memory);
        return NULL;
    }
    k->kernel = a;
    k->width = radius * 2 + 1;
    k->buffer = buf;
    k->radius = radius;
    return k;

}
void ConvolutionKernel_destroy(Context * context, ConvolutionKernel * kernel)
{
    if (kernel != NULL) {
        CONTEXT_free(context, kernel->kernel);
        CONTEXT_free(context, kernel->buffer);
        kernel->kernel = NULL;
        kernel->buffer = NULL;
    }
    CONTEXT_free(context, kernel);
}



ConvolutionKernel * ConvolutionKernel_create_guassian(Context * context, double stdDev, uint32_t radius)
{
    ConvolutionKernel * k = ConvolutionKernel_create(context, radius);
    if (k != NULL) {
        for (uint32_t i = 0; i < k->width; i++) {

            k->kernel[i] = (float)ir_guassian(abs((int)radius - (int)i), stdDev);
        }
    }
    return k;
}

double ConvolutionKernel_sum(ConvolutionKernel * kernel)
{
    double sum = 0;
    for (uint32_t i = 0; i < kernel->width; i++) {
        sum += kernel->kernel[i];
    }
    return sum;
}

void ConvolutionKernel_normalize(ConvolutionKernel * kernel, float desiredSum)
{
    double sum = ConvolutionKernel_sum(kernel);
    if (sum == 0) return; //nothing to do here, zeroes are as normalized as you can get ;)
    float factor = (float)(desiredSum / sum);
    for (uint32_t i = 0; i < kernel->width; i++) {
        kernel->kernel[i] *= factor;
    }
}
ConvolutionKernel * ConvolutionKernel_create_guassian_normalized(Context * context, double stdDev, uint32_t radius)
{
    ConvolutionKernel *kernel = ConvolutionKernel_create_guassian(context, stdDev, radius);
    if (kernel != NULL) {
        ConvolutionKernel_normalize(kernel, 1);
    }
    return kernel;
}

ConvolutionKernel * ConvolutionKernel_create_guassian_sharpen(Context * context, double stdDev, uint32_t radius)
{
    ConvolutionKernel *kernel = ConvolutionKernel_create_guassian(context, stdDev, radius);
    if (kernel != NULL) {
        double sum = ConvolutionKernel_sum(kernel);
        for (uint32_t i = 0; i < kernel->width; i++) {
            if (i == radius) {
                kernel->kernel[i] = (float)(2 * sum - kernel->kernel[i]);
            } else {
                kernel->kernel[i] *= -1;
            }
        }
        ConvolutionKernel_normalize(kernel, 1);
    }
    return kernel;
}


bool BitmapFloat_convolve_rows(Context * context, BitmapFloat * buf,  ConvolutionKernel *kernel, uint32_t convolve_channels, uint32_t from_row, int row_count)
{

    const uint32_t radius = kernel->radius;
    const float threshold_min = kernel->threshold_min_change;
    const float threshold_max = kernel->threshold_max_change;

    //Do nothing unless the image is at least half as wide as the kernel.
    if (buf->w < radius + 1) return true;

    const uint32_t buffer_count = radius + 1;
    const uint32_t w = buf->w;
    const int32_t int_w = (int32_t)buf->w;
    const uint32_t step = buf->channels;

    const uint32_t until_row = row_count < 0 ? buf->h : from_row + (unsigned)row_count;

    const uint32_t ch_used = convolve_channels;

    float* __restrict buffer =  kernel->buffer;
    float* __restrict avg = &kernel->buffer[buffer_count * ch_used];

    const float  * __restrict kern = kernel->kernel;

    const int  wrap_mode = 0;

    for (uint32_t row = from_row; row < until_row; row++) {

        float* __restrict source_buffer = &buf->pixels[row * buf->float_stride];
        int circular_idx = 0;

        for (uint32_t ndx = 0; ndx < w + buffer_count; ndx++) {
            //Flush old value
            if (ndx >= buffer_count) {
                memcpy(&source_buffer[(ndx - buffer_count) * step], &buffer[circular_idx * ch_used], ch_used * sizeof(float));
            }
            //Calculate and enqueue new value
            if (ndx < w) {
                const int left = ndx - radius;
                const int right = ndx + radius;
                int i;

                memset(avg, 0, sizeof(float) * ch_used);

                if (left < 0 || right >= (int32_t)w) {
                    if (wrap_mode == 0){
                        //Only sample what's present, and fix the average later.
                        float total_weight = 0;
                        /* Accumulate each channel */
                        for (i = left; i <= right; i++) {
                            if (i > 0 && i < int_w){
                                const float weight = kern[i - left];
                                total_weight += weight;
                                for (uint32_t j = 0; j < ch_used; j++)
                                    avg[j] += weight * source_buffer[i * step + j];
                            }
                        }
                        for (uint32_t j = 0; j < ch_used; j++)
                            avg[j] = avg[j] / total_weight;
                    }
                    else if (wrap_mode == 1){
                        //Extend last pixel to be used for all missing inputs
                        /* Accumulate each channel */
                        for (i = left; i <= right; i++) {
                            const float weight = kern[i - left];
                            const uint32_t ix = EVIL_CLAMP (i, 0, int_w - 1);
                            for (uint32_t j = 0; j < ch_used; j++)
                                avg[j] += weight * source_buffer[ix * step + j];
                        }
                    }
                } else {
                    /* Accumulate each channel */
                    for (i = left; i <= right; i++) {
                        const float weight = kern[i - left];
                        for (uint32_t j = 0; j < ch_used; j++)
                            avg[j] += weight * source_buffer[i * step + j];
                    }
                }

                //Enqueue difference
                memcpy(&buffer[circular_idx * ch_used], avg, ch_used * sizeof(float));

                if (threshold_min > 0 || threshold_max > 0) {
                    float change = 0;
                    for (uint32_t j = 0; j < ch_used; j++)
                        change += (float)fabs(source_buffer[ndx * step + j] - avg[j]);

                    if (change < threshold_min || change > threshold_max) {
                        memcpy(&buffer[circular_idx * ch_used], &source_buffer[ndx * step], ch_used * sizeof(float));
                    }
                }
            }
            circular_idx = (circular_idx + 1) % buffer_count;

        }
    }
    return true;
}


bool BitmapFloat_boxblur_rows (Context * context, BitmapFloat * buf, uint32_t radius, uint32_t passes, const uint32_t convolve_channels, ConvolutionKernel *kernel, uint32_t from_row, int row_count)
{
    const uint32_t buffer_count = radius + 1;
    const uint32_t w = buf->w;
    const uint32_t step = buf->channels;
    const uint32_t until_row = row_count < 0 ? buf->h : from_row + (unsigned)row_count;
    const uint32_t ch_used = buf->channels;
    float* __restrict buffer = kernel->buffer;

    const int std_count = radius * 2 + 1;
    const float std_factor = 1.0 / (float)(std_count);

    for (uint32_t row = from_row; row < until_row; row++) {

        float* __restrict source_buffer = &buf->pixels[row * buf->float_stride];

        for (uint32_t pass_index = 0; pass_index < passes; pass_index++){
            int circular_idx = 0;

            float sum[4] = {0, 0, 0, 0};
            uint32_t count = 0;

            for (uint32_t ndx = 0; ndx < radius; ndx++) {
                for (uint32_t ch = 0; ch < convolve_channels; ch++){
                    sum[ch] += source_buffer[ndx * step + ch];
                }
                count++;
            }


            for (uint32_t ndx = 0; ndx < w + buffer_count; ndx++) { //Pixels

                if (ndx >= buffer_count) { // same as ndx > radius
                    //Remove trailing item from average
                    for (uint32_t ch = 0; ch < convolve_channels; ch++){
                        sum[ch] -= source_buffer[(ndx - radius - 1) * step + ch];
                    }
                    count--;

                    //Flush old value
                    memcpy (&source_buffer[(ndx - buffer_count) * step], &buffer[circular_idx * ch_used], ch_used * sizeof (float));

                }
                //Calculate and enqueue new value
                if (ndx < w) {
                    if (ndx < w - radius){
                        for (uint32_t ch = 0; ch < convolve_channels; ch++){
                            sum[ch] += source_buffer[(ndx + radius) * step + ch];
                        }
                        count++;
                    }

                    //Enqueue averaged value
                    if (count != std_count){
                        for (uint32_t ch = 0; ch < convolve_channels; ch++){
                            buffer[circular_idx * ch_used + ch] = sum[ch]  / (float)count; //Recompute factor
                        }
                    }
                    else{

                        for (uint32_t ch = 0; ch < convolve_channels; ch++){
                            buffer[circular_idx * ch_used + ch] = sum[ch] * std_factor;
                        }
                    }
                }
                circular_idx = (circular_idx + 1) % buffer_count;

            }
        }
    }
    return true;
}


bool BitmapFloat_boxblur_misaligned_rows (Context * context, BitmapFloat * buf, uint32_t radius, uint32_t align, const uint32_t convolve_channels, ConvolutionKernel *kernel, uint32_t from_row, int row_count)
{
    if (align != 1 && align != -1){
        CONTEXT_error (context, Invalid_internal_state);
        return false;
    }

    const uint32_t buffer_count = radius + 2;
    const uint32_t w = buf->w;
    const uint32_t step = buf->channels;
    const uint32_t until_row = row_count < 0 ? buf->h : from_row + (unsigned)row_count;
    const uint32_t ch_used = buf->channels;
    float* __restrict buffer = kernel->buffer;

    const uint32_t write_offset = align == -1 ? -1 : 0;

    for (uint32_t row = from_row; row < until_row; row++) {
        float* __restrict source_buffer = &buf->pixels[row * buf->float_stride];
        int circular_idx = 0;

        float sum[4] = {0, 0, 0, 0};
        float count = 0;

        for (uint32_t ndx = 0; ndx < radius; ndx++) {
            float factor = (ndx == radius - 1) ? 0.5 : 1;
            for (uint32_t ch = 0; ch < convolve_channels; ch++){
                sum[ch] += source_buffer[ndx * step + ch] * factor;
            }
            count += factor;
        }

        for (uint32_t ndx = 0; ndx < w + buffer_count; ndx++) { //Pixels

            //Calculate new value
            if (ndx < w) {
                if (ndx < w - radius){
                    for (uint32_t ch = 0; ch < convolve_channels; ch++){
                        sum[ch] += source_buffer[(ndx + radius) * step + ch] * 0.5;
                    }
                    count+= 0.5;
                }
                if (ndx - 1 < w - radius){
                    for (uint32_t ch = 0; ch < convolve_channels; ch++){
                        sum[ch] += source_buffer[(ndx - 1 + radius) * step + ch] * 0.5;
                    }
                    count += 0.5;
                }

                //Remove trailing items from average
                if (ndx >= radius){

                    for (uint32_t ch = 0; ch < convolve_channels; ch++){
                        sum[ch] -= source_buffer[(ndx - radius) * step + ch] * 0.5;
                    }
                    count-= 0.5;
                }
                if (ndx - 1 >= radius ){
                    for (uint32_t ch = 0; ch < convolve_channels; ch++){
                        sum[ch] -= source_buffer[(ndx - 1 - radius) * step + ch] * 0.5;
                    }
                    count -= 0.5;
                }

            }
            //Flush old value
            if (ndx + write_offset  >= buffer_count) {
                memcpy (&source_buffer[(ndx + write_offset - buffer_count) * step], &buffer[circular_idx * ch_used], ch_used * sizeof (float));

            }
            //enqueue new value
            if (ndx < w) {
                for (uint32_t ch = 0; ch < convolve_channels; ch++){
                    buffer[circular_idx * ch_used + ch] = sum[ch] / (float)count;
                }
            }
            circular_idx = (circular_idx + 1) % buffer_count;

        }
    }

    return true;
}




bool BitmapFloat_approx_gaussian_blur_rows (Context * context, BitmapFloat * buf, float sigma, ConvolutionKernel *kernel, uint32_t from_row, int row_count)
{
     //http://www.w3.org/TR/SVG11/filters.html#feGaussianBlur
    // For larger values of 's' (s >= 2.0), an approximation can be used :
    // Three successive box - blurs build a piece - wise quadratic convolution kernel, which approximates the Gaussian kernel to within roughly 3 % .

    uint32_t d = (int)floorf (1.8799712059732503768118239636082839397552400554574537 * sigma + 0.5);

    d = umin (d, (buf->w - 1) / 2);//Never exceed half the size of the buffer.


    const uint32_t max_radius = ((d + 1) / 2);
    //Ensure the buffer is large enough
    if (max_radius > kernel->radius){
        CONTEXT_error (context, StatusCode::Invalid_internal_state);
        return false;
    }

    if (!BitmapFloat_boxblur_misaligned_rows (context, buf, max_radius, -1, buf->channels, kernel, from_row, row_count)){
        CONTEXT_error_return (context);
    }
    return true;

    //... if d is odd, use three box - blurs of size 'd', centered on the output pixel.
    if (d % 2 > 0){
        if (!BitmapFloat_boxblur_rows (context, buf, d / 2, 3, buf->channels,  kernel, from_row, row_count)){
            CONTEXT_error_return (context);
        }
    }
    else{
        // ... if d is even, two box - blurs of size 'd'
        // (the first one centered on the pixel boundary between the output pixel and the one to the left,
        //  the second one centered on the pixel boundary between the output pixel and the one to the right)
        // and one box blur of size 'd+1' centered on the output pixel.

        if (!BitmapFloat_boxblur_misaligned_rows (context, buf, max_radius, -1, buf->channels, kernel, from_row, row_count)){
            CONTEXT_error_return (context);
        }

        if (!BitmapFloat_boxblur_misaligned_rows (context, buf, max_radius, 1, buf->channels, kernel, from_row, row_count)){
            CONTEXT_error_return (context);
        }
        if (!BitmapFloat_boxblur_rows (context, buf, max_radius, 1, buf->channels == 4, kernel, from_row, row_count)){
            CONTEXT_error_return (context);
        }
    }

    return true;
}



/*
static void BgraSharpenInPlaceX(BitmapBgra * im, float pct)
{
    const float n = -pct / (pct - 1); //if 0 < pct < 1
    const float outer_coeff = n / -2.0f;
    const float inner_coeff = n + 1;

    uint32_t y, current, prev, next;

    const uint32_t sy = im->h;
    const uint32_t stride = im->stride;
    const uint32_t bytes_pp = BitmapPixelFormat_bytes_per_pixel (im->fmt);


    if (pct <= 0 || im->w < 3 || bytes_pp < 3) return;

    for (y = 0; y < sy; y++)
    {
        unsigned char *row = im->pixels + y * stride;
        for (current = bytes_pp, prev = 0, next = bytes_pp + bytes_pp; next < stride; prev = current, current = next, next += bytes_pp){
            //We never sharpen the alpha channel
            //TODO - we need to buffer the left pixel to prevent it from affecting later calculations
            for (uint32_t i = 0; i < 3; i++)
                row[current + i] = uchar_clamp_ff(outer_coeff * (float)row[prev + i] + inner_coeff * (float)row[current + i] + outer_coeff * (float)row[next + i]);
        }
    }
}
*/

static void
SharpenBgraFloatInPlace(float* buf, unsigned int count, double pct,
                        int step)
{

    const float n = (float)(-pct / (pct - 1)); //if 0 < pct < 1
    const float c_o = n / -2.0f;
    const float c_i = n + 1;

    unsigned int ndx;

    // if both have alpha, process it
    if (step == 4) {
        float left_b = buf[0 * 4 + 0];
        float left_g = buf[0 * 4 + 1];
        float left_r = buf[0 * 4 + 2];
        float left_a = buf[0 * 4 + 3];

        for (ndx = 1; ndx < count - 1; ndx++) {
            const float b = buf[ndx * 4 + 0];
            const float g = buf[ndx * 4 + 1];
            const float r = buf[ndx * 4 + 2];
            const float a = buf[ndx * 4 + 3];
            buf[ndx * 4 + 0] = left_b * c_o + b * c_i + buf[(ndx + 1) * 4 + 0] * c_o;
            buf[ndx * 4 + 1] = left_g * c_o + g * c_i + buf[(ndx + 1) * 4 + 1] * c_o;
            buf[ndx * 4 + 2] = left_r * c_o + r * c_i + buf[(ndx + 1) * 4 + 2] * c_o;
            buf[ndx * 4 + 3] = left_a * c_o + a * c_i + buf[(ndx + 1) * 4 + 3] * c_o;
            left_b = b;
            left_g = g;
            left_r = r;
            left_a = a;
        }
    }
    // otherwise do the same thing without 4th chan
    // (ifs in loops are expensive..)
    else {
        float left_b = buf[0 * 3 + 0];
        float left_g = buf[0 * 3 + 1];
        float left_r = buf[0 * 3 + 2];

        for (ndx = 1; ndx < count - 1; ndx++) {
            const float b = buf[ndx * 3 + 0];
            const float g = buf[ndx * 3 + 1];
            const float r = buf[ndx * 3 + 2];
            buf[ndx * 3 + 0] = left_b * c_o + b * c_i + buf[(ndx + 1) * 3 + 0] * c_o;
            buf[ndx * 3 + 1] = left_g * c_o + g * c_i + buf[(ndx + 1) * 3 + 1] * c_o;
            buf[ndx * 3 + 2] = left_r * c_o + r * c_i + buf[(ndx + 1) * 3 + 2] * c_o;
            left_b = b;
            left_g = g;
            left_r = r;
        }

    }

}





bool BitmapFloat_sharpen_rows(Context * context, BitmapFloat * im, uint32_t start_row, uint32_t row_count, double pct)
{
    if (!(start_row + row_count <= im->h)) {
        CONTEXT_error(context, Invalid_internal_state);
        return false;
    }
    for (uint32_t row = start_row; row < start_row + row_count; row++) {
        SharpenBgraFloatInPlace(im->pixels + (im->float_stride * row), im->w, pct, im->channels);
    }
    return true;
}
