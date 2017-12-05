#include "hls_fast_local_laplacian.h"

#include "hls_opencv.h"		// cvMat2hlsMat() etc.

#ifdef _WIN32

#include "ap_int.h"
//#include "ap_fixed.h"

#include "hls_stream.h"

#include "hls/utils/x_hls_utils.h"
#include "hls/utils/x_hls_traits.h"
#include "hls/utils/x_hls_defines.h"

#include "hls/hls_video_types.h"
#include "hls/hls_video_mem.h"
#include "hls/hls_video_core.h"
#include "hls/hls_video_imgbase.h"
#include "hls/hls_video_io.h"

//#include "hls_math.h"

#define ___HLS__VIDEO__
#include "hls/hls_video_imgproc.h"

#else

#include "hls_video.h"

#endif

void my_split(
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& src,
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& dst1,
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& dst2);


void hls_local_laplacian_wrap(cv::Mat& src, cv::Mat& dst, float sigma, float fact, int N)
{
	// Check input
	if (N <= 0) {
		return;
	}

	// Settings
	// num_levels: Max. 9 (for 1024 x 1024 image)
	int num_levels = 4;// std::ceil(std::log(std::min(src.rows, src.cols)) - log(2)) + 2;
//	float discretisation_step = 1.0f / (N - 1);

	// Original image
	data_in_t*  buf_src;
	data_out_t* buf_dst;
	buf_src = new data_in_t[src.rows*src.cols];
	buf_dst = new data_out_t[src.rows*src.cols];

	memcpy(buf_src, src.data, src.rows*src.cols*sizeof(data_in_t));


	// List for pyramid's widths & heights
	int pyr_rows[_MAX_LEVELS_] = { 0 };
	int pyr_cols[_MAX_LEVELS_] = { 0 };

	// Total memory size for pyramids (measured in num. of elements)
	int sz_gaussian_pyr = 0;
	int sz_laplacian_pyr = 0;

	int width = src.cols, height = src.rows;
	for (int i = 0; i < num_levels; i++) {
		pyr_cols[i] = width;
		pyr_rows[i] = height;

		sz_gaussian_pyr += width*height;
		sz_laplacian_pyr += width*height;

		height = std::ceil(height / 2.0);
		width = std::ceil(width / 2.0);
	}


	// Pyramids
	float* input_gaussian_pyr = NULL;
	float* output_laplace_pyr = NULL;
	input_gaussian_pyr = new float [sz_gaussian_pyr];
	output_laplace_pyr = new float [sz_laplacian_pyr];


	// Construct Laplacian pyramid
	laplacian_pyramid(buf_src, output_laplace_pyr, num_levels, pyr_rows, pyr_cols);
#if 0
	{
		// Show pyramid image
		int h_ = src.rows, w_ = src.cols;
		int offset = 0;
		for (int l = 0; l < num_levels; l++) {
			std::string name = "L - ";
			name += std::to_string(l);

			cv::Mat tmp(h_, w_, CV_32FC1);
			tmp.data = (unsigned char*)(&output_laplace_pyr[offset]);
			cv::imshow(name, tmp + 0.5);
			cv::waitKey(1.0 * 1000);
			cv::destroyWindow(name);

			offset += h_*w_;

			h_ = std::ceil(h_ / 2.0);
			w_ = std::ceil(w_ / 2.0);
		}
	}
#endif

	// Gaussian Pyramid
	// Copy finest level
	memcpy(input_gaussian_pyr, buf_src, src.rows*src.cols * sizeof(float));
	gaussian_pyramid(buf_src, input_gaussian_pyr, num_levels, pyr_rows, pyr_cols);
#if 0
	{
		// Show pyramid image
		int h_ = src.rows, w_ = src.cols;
		int offset = 0;
		for (int l = 0; l < num_levels; l++) {
			std::string name = "G - ";
			name += std::to_string(l);

			cv::Mat tmp(h_, w_, CV_32FC1);
			tmp.data = (unsigned char*)(&input_gaussian_pyr[offset]);
			cv::imshow(name, tmp);
			cv::waitKey(1.0 * 1000);
			cv::destroyWindow(name);

			offset += h_*w_;

			h_ = std::ceil(h_ / 2.0);
			w_ = std::ceil(w_ / 2.0);
		}
	}
#endif

	hls_local_laplacian(
		buf_src, input_gaussian_pyr, output_laplace_pyr, pyr_rows, pyr_cols,
		num_levels, sigma, fact, N);
#if 0
	{
		// Show pyramid image
		int h_ = src.rows, w_ = src.cols;
		int offset = 0;
		for (int l = 0; l < num_levels; l++) {
			std::string name = "L - ";
			name += std::to_string(l);

			cv::Mat tmp(h_, w_, CV_32FC1);
			tmp.data = (unsigned char*)(&output_laplace_pyr[offset]);
			cv::imshow(name, tmp + 0.5);
			cv::waitKey(1.0 * 1000);
			cv::destroyWindow(name);

			offset += h_*w_;

			h_ = std::ceil(h_ / 2.0);
			w_ = std::ceil(w_ / 2.0);
		}
	}
#endif

	// Reconstruct
	reconstruct(output_laplace_pyr, buf_dst, num_levels, pyr_rows, pyr_cols);

	// Copy back
	dst.create(src.rows, src.cols, src.type());
	memcpy(dst.data, buf_dst, dst.rows*dst.cols * sizeof(data_out_t));

	// Release memory
	if (input_gaussian_pyr) {
		delete [] input_gaussian_pyr;
	}
	if (output_laplace_pyr) {
		delete [] output_laplace_pyr;
	}

	if (buf_src) {
		delete [] buf_src;
	}
	if (buf_dst) {
		delete [] buf_dst;
	}
}


// Accelerated function
// I:    Original image
// gau:  Pre-built Gaussian pyramid
// dst:  Remapped Laplacian pyramid
void hls_local_laplacian(float* I, float* gau, float* dst,
		int pyr_rows[_MAX_LEVELS_], int pyr_cols[_MAX_LEVELS_],
		int num_levels, float sigma, float fact, int N)
{
	float discretisation_step = 1.0f / (N - 1);

	int sz_temp_pyr = 0;
	for (int i = 0; i < num_levels; i++) {
		sz_temp_pyr += pyr_rows[i] * pyr_cols[i];
	}

	float* temp_laplace_pyr = NULL;
	temp_laplace_pyr = new float [sz_temp_pyr];

	// Copy
	int offset2 = 0;
	for (int l = 0; l < num_levels - 1; l++) {
		offset2 += pyr_rows[l] * pyr_cols[l];
	}

	for (int r = 0; r < pyr_rows[num_levels - 1]; r++) {
		for (int c = 0; c < pyr_cols[num_levels - 1]; c++) {
			dst[offset2 + r*pyr_cols[num_levels - 1] + c] = gau[offset2 + r*pyr_cols[num_levels - 1] + c];
		}
	}

	// Parallelize-able
	int rows = pyr_rows[0];
	int cols = pyr_cols[0];

	float* I_remap = NULL;
	I_remap = new float [rows*cols];
	for (int i = 0; i < N; i++) {
		float ref = ((float)i) / ((float)(N - 1));

		// Remap original image
		remap(I, I_remap, ref, fact, sigma, rows, cols);

		// Create laplacian pyramid from remapped image
		laplacian_pyramid(I_remap, temp_laplace_pyr, num_levels, pyr_rows, pyr_cols);

		int offset = 0;
		for (int level = 0; level < num_levels - 1; level++) {
			float x_ = 0;
			for (int r = 0; r < pyr_rows[level]; r++) {
				for (int c = 0; c < pyr_cols[level]; c++) {
					if (std::abs(gau[offset + r*pyr_cols[level] + c] - ref) < discretisation_step) {
						x_ = 1 - std::abs(gau[offset + r*pyr_cols[level] + c] - ref) / discretisation_step;
						x_ = x_ * temp_laplace_pyr[offset + r*pyr_cols[level] + c];
					}
					else {
						x_ = 0;
					}
					x_ = x_ + dst[offset + r*pyr_cols[level] + c];

					dst[offset + r*pyr_cols[level] + c] = x_;
				}
			}

			offset += pyr_rows[level] * pyr_cols[level];
		}
	}

	// Release memory
	if(temp_laplace_pyr){
		delete [] temp_laplace_pyr;
	}

	delete [] I_remap;
}

void downsample(
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& src,
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& dst)
{
//#pragma HLS DATAFLOW

	// Convolution Kernel
	// This sums to unity
	static const float x[25] = {
		0.0025, 0.0125, 0.0200, 0.0125, 0.0025,
		0.0125, 0.0625, 0.1000, 0.0625, 0.0125,
		0.0200, 0.1000, 0.1600, 0.1000, 0.0200,
		0.0125, 0.0625, 0.1000, 0.0625, 0.0125,
		0.0025, 0.0125, 0.0200, 0.0125, 0.0025 };
	hls::Window<5, 5, float> kernel;
	for (int r = 0; r < 5; r++) {
		for (int c = 0; c < 5; c++) {
//#pragma HLS PIPELINE
			kernel.val[r][c] = x[r * 5 + c];
		}
	}

	// Convolve
	hls::Point p(-1, -1);
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_> tmp(src.rows, src.cols);
	hls::Filter2D(src, tmp, kernel, p);

	// Decimate
	hls::Scalar<HLS_MAT_CN(_MAT_TYPE_), HLS_TNAME(_MAT_TYPE_)> px;
	for (int r = 0; r < src.rows; r++) {
//#pragma HLS PIPELINE
		for (int c = 0; c < src.cols; c++) {
//#pragma HLS PIPELINE
//#pragma HLS LOOP_TRIPCOUNT max=1024
			tmp >> px;
			if ( (r % 2 == 0) && (c % 2 == 0) ) {
				dst << px;
			}
		}
	}
}

void upsample(
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& src,
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& dst,
	int rows, int cols)
{
	// Convolution Kernel
	// This sums to unity
	static const float x[25] = {
		0.0025, 0.0125, 0.0200, 0.0125, 0.0025,
		0.0125, 0.0625, 0.1000, 0.0625, 0.0125,
		0.0200, 0.1000, 0.1600, 0.1000, 0.0200,
		0.0125, 0.0625, 0.1000, 0.0625, 0.0125,
		0.0025, 0.0125, 0.0200, 0.0125, 0.0025 };
	hls::Window<5, 5, float> kernel;
	for (int r = 0; r < 5; r++) {
		for (int c = 0; c < 5; c++) {
//#pragma HLS PIPELINE
			kernel.val[r][c] = x[r * 5 + c];
		}
	}

	// Up-scaling
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_> tmp(rows, cols);
	hls::Scalar<HLS_MAT_CN(_MAT_TYPE_), HLS_TNAME(_MAT_TYPE_)> px;
	hls::Window<1, _MAX_ROWS_, HLS_TNAME(_MAT_TYPE_)> buf;	// Line buffer
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if ((r % 2 == 0) && (c % 2 == 0)) {
				src >> px;
			}

			if (r % 2 == 0) {
				tmp << px;
				buf.val[0][c] = px.val[0];
			}
			else {
				tmp << buf.val[0][c];
			}
		}
	}

	// Convolve
	hls::Point p(-1, -1);
	hls::Filter2D(tmp, dst, kernel, p);
}

// Marked for HW acceleration
void gaussian_pyramid(float* src, float* dst, int num_levels,
		int pyr_rows[_MAX_LEVELS_], int pyr_cols[_MAX_LEVELS_])
{
	// Check range of input for determining trip count
	assert(num_levels <= _MAX_LEVELS_);

	// Inter-loop buffer
	hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> buf_;
	hls::Scalar<1, float> px;

	int offset = 0;

	for (int l = 1; l < num_levels; l++) {
		int rows_ = pyr_rows[l - 1];
		int cols_ = pyr_cols[l - 1];

		offset += rows_*cols_;

		// Before downsampling
		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> in(rows_, cols_);

		if (l == 1) {
			// Copy from source
			for (int r = 0; r < rows_; r++) {
				for (int c = 0; c < cols_; c++) {
					px.val[0] = src[r*cols_ + c];
					in << px;
				}
			}
		}
		else {
			// Copy from inter-loop buffer
			for (int r = 0; r < rows_; r++) {
				for (int c = 0; c < cols_; c++) {
					buf_ >> px;
					in << px;
				}
			}
		}

		// Image size after down sampling
		int rows2_ = pyr_rows[l];
		int cols2_ = pyr_cols[l];

		// Perform down-sampling
		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> out(rows2_, cols2_);
		downsample(in, out);

		// Transfer data - Add to pyramid
		buf_.init(rows2_, cols2_);
		for (int r = 0; r < rows2_; r++) {
			for (int c = 0; c < cols2_; c++) {
				out >> px;

				// Output
				dst[offset + r*cols2_ + c] = px.val[0];

				// For next loop
				if (l != num_levels - 1) {
					// Prevent remaining data
					buf_ << px;
				}
			}
		}

#if 0
		// Debugging
		cv::Mat tmp(rows2_, cols2_, CV_32FC1);
		tmp.data = (unsigned char*)(dst[i]);
		cv::imshow("Down", tmp);
		cv::waitKey();
		cv::destroyWindow("Down");
#endif
	}
}

void laplacian_pyramid(float* src, float* dst, int num_levels,
		int pyr_rows[_MAX_LEVELS_], int pyr_cols[_MAX_LEVELS_])
{
	// Check range of input for determining trip count
	assert(num_levels <= _MAX_LEVELS_);
	
	// Inter-loop buffer
	hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> buf_;
	hls::Scalar<1, float> px;

	int offset = 0;

	for (int l = 0; l < num_levels - 1; l++) {
		int rows_ = pyr_rows[l];
		int cols_ = pyr_cols[l];

		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> in(rows_, cols_);	// for downsampling
		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> in2(rows_, cols_);	// 

		if (l == 0) {
			// Copy from source
			for (int r = 0; r < rows_; r++) {
				for (int c = 0; c < cols_; c++) {
					px.val[0] = src[r*cols_ + c];
					in << px;
					in2 << px;
				}
			}
		}
		else {
			// Copy from inter-loop buffer
			for (int r = 0; r < rows_; r++) {
				for (int c = 0; c < cols_; c++) {
					buf_ >> px;
					in << px;
					in2 << px;
				}
			}
		}

		// Down-sample
		int rows2_ = pyr_rows[l + 1];
		int cols2_ = pyr_cols[l + 1];

		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> out_down(rows2_, cols2_);	// For down-sampling
		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> out_down2(rows2_, cols2_);	// For up-sampling
		downsample(in, out_down);
		
		buf_.init(rows2_, cols2_);

		my_split(out_down, buf_, out_down2);

		// Up-sample
		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> out_up(rows_, cols_);
		upsample(out_down2, out_up, rows_, cols_);

		// Diff
		hls::Scalar<1, float> px0, px1;
		hls::Mat<_MAX_ROWS_, _MAX_ROWS_, _MAT_TYPE_> diff(rows_, cols_);
		for (int r = 0; r < rows_; r++) {
			for (int c = 0; c < cols_; c++) {
				in2 >> px0;
				out_up >> px1;

				diff << (px0 - px1);
			}
		}

		// Transfer
		for (int r = 0; r < rows_; r++) {
			for (int c = 0; c < cols_; c++) {
				diff >> px;
				dst[offset + r*cols_ + c] = px.val[0];
			}
		}

		offset += rows_*cols_;
	}

	// Transfer last layer
	int rows_ = pyr_rows[num_levels - 1];
	int cols_ = pyr_cols[num_levels - 1];
	for (int r = 0; r < rows_; r++) {
		for (int c = 0; c < cols_; c++) {
			buf_ >> px;
			dst[offset + r*cols_ + c] = px.val[0];
		}
	}
}

void reconstruct(float* src, data_out_t* dst, int num_levels, int pyr_rows[_MAX_LEVELS_], int pyr_cols[_MAX_LEVELS_])
{
	// Inter-loop buffer
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_> buf_;	// Inter-loop buffer
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_> in(pyr_rows[num_levels - 1], pyr_cols[num_levels - 1]);
	hls::Scalar<1, float> px;

	// Last layer in the pyramid
	int offset = 0;
	for (int l = 0; l < num_levels - 1; l++) {
		offset += pyr_rows[l] * pyr_cols[l];
	}

	for (int r = 0; r < pyr_rows[num_levels - 1]; r++) {
		for (int c = 0; c < pyr_cols[num_levels - 1]; c++) {
			px.val[0] = src[offset + r*pyr_cols[num_levels - 1] + c];
			in << px;
		}
	}

	hls::Scalar<1, float> px2;
	for (int i = num_levels - 2; i >= 0; i--) {
		hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_> out(pyr_rows[i], pyr_cols[i]);

		offset -= pyr_rows[i] * pyr_cols[i];

		// Upsample
		if (i == num_levels - 2) {
			upsample(in, out, pyr_rows[i], pyr_cols[i]);
		}
		else {
			upsample(buf_, out, pyr_rows[i], pyr_cols[i]);
		}

		buf_.init(pyr_rows[i], pyr_cols[i]);
		
		// Load data
		for (int r = 0; r < pyr_rows[i]; r++) {
			for (int c = 0; c < pyr_cols[i]; c++) {
				out >> px;
				px.val[0] += src[offset + r*pyr_cols[i] + c];
				buf_ << px;
			}
		}
	}

	// Output
	for (int r = 0; r < pyr_rows[0]; r++) {
		for (int c = 0; c < pyr_cols[0]; c++) {
			buf_ >> px;
			dst[r*pyr_cols[0] + c] = px.val[0];
		}
	}
}

void remap(float* src, float* dst, float ref, float fact, float sigma, int rows, int cols)
{
	float I;
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			I = src[r*cols + c];
#ifdef _WIN32
			dst[r*cols + c] =
				fact*(I - ref)*std::exp(-(I - ref)*(I - ref) / (2 * sigma*sigma)); 
#else
			dst[r*cols + c] =
				fact*(I - ref)*hls::expf(-(I - ref)*(I - ref) / (2 * sigma*sigma));
#endif
		}
	}
}

void my_split(
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& src,
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& dst1,
	hls::Mat<_MAX_ROWS_, _MAX_COLS_, _MAT_TYPE_>& dst2)
{
	int rows_ = src.rows;
	int cols_ = src.cols;

	assert(rows_ <= _MAX_ROWS_);
	assert(cols_ <= _MAX_COLS_);

	for (int r = 0; r < rows_; r++) {
		for (int c = 0; c < cols_; c++) {
//#pragma HLS PIPELINE
			hls::Scalar<HLS_MAT_CN(_MAT_TYPE_), HLS_TNAME(_MAT_TYPE_)> px;
			src >> px;

			dst1 << px;
			dst2 << px;
		}
	}
}
