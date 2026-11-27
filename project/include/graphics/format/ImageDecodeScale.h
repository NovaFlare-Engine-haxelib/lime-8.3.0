#ifndef LIME_GRAPHICS_FORMAT_IMAGE_DECODE_SCALE_H
#define LIME_GRAPHICS_FORMAT_IMAGE_DECODE_SCALE_H

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdint.h>

namespace lime {


	struct ImageDecodeScale {

		int maximum;
		int threshold;
		double ratio;
		int sourceWidth;
		int sourceHeight;
		int width;
		int height;
		bool allocationFailed;
		bool pngRecognized;

		bool SetTarget (uint32_t originalWidth, uint32_t originalHeight) {

			if (!originalWidth || !originalHeight || originalWidth > INT_MAX / 4 || originalHeight > INT_MAX) return false;

			sourceWidth = originalWidth;
			sourceHeight = originalHeight;
			int longest = (std::max) (sourceWidth, sourceHeight);
			double scale = longest >= threshold && std::isfinite (ratio) ? std::sqrt ((std::max) (0.1, (std::min) (ratio, 1.0))) : 1.0;
			if (maximum > 0) scale = (std::min) (scale, (double)maximum / longest);
			width = (std::max) (1, (int)std::floor (sourceWidth * scale + 1e-9));
			height = (std::max) (1, (int)std::floor (sourceHeight * scale + 1e-9));
			if (maximum > 0) {
				width = (std::min) (width, maximum);
				height = (std::min) (height, maximum);
			}
			return (width != sourceWidth || height != sourceHeight) && height <= INT_MAX / 4 / width;

		}

	};


	class ScaleImageRows {

		public:

			ScaleImageRows (int inputWidth, int inputHeight, const ImageDecodeScale& target, unsigned char *pixels)
				: inputWidth (inputWidth), inputHeight (inputHeight), target (target), pixels (pixels),
				  stepX ((double)inputWidth / target.width), stepY ((double)inputHeight / target.height), row (0), transparent (false) {}

			void Read (const unsigned char *previous, const unsigned char *current, int inputRow) {

				while (row < target.height) {

					double sampleY = (row + 0.5) * stepY - 0.5;
					int top = (int)sampleY;
					int bottom = (std::min) (top + 1, inputHeight - 1);
					if (bottom != inputRow) return;
					Write (top == bottom ? current : previous, current, sampleY - top);
					row++;
				}

			}

			bool IsComplete () const { return row == target.height; }
			bool IsTransparent () const { return transparent; }

		private:

			void Write (const unsigned char *upper, const unsigned char *lower, double vertical) {

				for (int column = 0; column < target.width; column++) {
					double sampleX = (column + 0.5) * stepX - 0.5;
					int left = (int)sampleX * 4;
					int right = (std::min) (left + 4, inputWidth * 4 - 4);
					double horizontal = sampleX - (int)sampleX;
					const unsigned char *neighbors[] = {upper + left, upper + right, lower + left, lower + right};
					double weights[] = {(1 - horizontal) * (1 - vertical), horizontal * (1 - vertical),
						(1 - horizontal) * vertical, horizontal * vertical};
					double alpha = 0;
					for (int neighbor = 0; neighbor < 4; neighbor++) {
						weights[neighbor] *= neighbors[neighbor][3];
						alpha += weights[neighbor];
					}
					for (int channel = 0; channel < 3; channel++) {
						double value = 0;
						for (int neighbor = 0; neighbor < 4; neighbor++) value += neighbors[neighbor][channel] * weights[neighbor];
						pixels[2 - channel] = (unsigned char)(value / 255 + 0.5);
					}
					pixels[3] = (unsigned char)(alpha + 0.5);
					if (pixels[3] != 255) transparent = true;
					pixels += 4;
				}

			}

			int inputWidth;
			int inputHeight;
			const ImageDecodeScale& target;
			unsigned char *pixels;
			double stepX;
			double stepY;
			int row;
			bool transparent;

	};


}

#endif
