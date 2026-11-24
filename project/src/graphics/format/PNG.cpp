#include <spng.h>
#include <graphics/format/PNG.h>
#include <graphics/ImageBuffer.h>
#include <system/System.h>
#include <utils/Bytes.h>
#include <memory>


namespace lime {


	static int ReadPngStream (spng_ctx*, void *user, void *destination, size_t length) {

		return lime::fread (destination, 1, length, (FILE_HANDLE*)user) == length ? SPNG_OK : SPNG_IO_EOF;

	}


	static bool CheckDecodeResult (int result, int expected, ImageDecodeScale& scale) {

		if (result == SPNG_EMEM) scale.allocationFailed = true;
		return result == expected;

	}


	static bool DecodeScaled (spng_ctx *ctx, ImageBuffer *imageBuffer, ImageDecodeScale& scale) {

		size_t rowBytes = (size_t)scale.sourceWidth * 4;
		std::unique_ptr<unsigned char, decltype (&free)> rows ((unsigned char*)malloc (rowBytes * 2), free);
		if (!rows) {
			scale.allocationFailed = true;
			return false;
		}

		int result = spng_decode_image (ctx, NULL, 0, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS | SPNG_DECODE_PROGRESSIVE);
		if (!CheckDecodeResult (result, SPNG_OK, scale)) return false;

		imageBuffer->Resize (scale.width, scale.height, 32);
		if (!imageBuffer->data || !imageBuffer->data->buffer || !imageBuffer->data->buffer->b) {
			scale.allocationFailed = true;
			return false;
		}
		ScaleImageRows scaler (scale.sourceWidth, scale.sourceHeight, scale, imageBuffer->data->buffer->b);
		unsigned char *current = rows.get ();
		unsigned char *previous = current + rowBytes;

		for (int row = 0; row < scale.sourceHeight; row++) {
			result = spng_decode_row (ctx, current, rowBytes);
			if (!CheckDecodeResult (result, row == scale.sourceHeight - 1 ? SPNG_EOI : SPNG_OK, scale)) return false;
			scaler.Read (previous, current, row);
			std::swap (current, previous);
		}

		imageBuffer->format = BGRA32;
		imageBuffer->premultiplied = true;
		imageBuffer->transparent = scaler.IsTransparent ();
		return scaler.IsComplete ();

	}


	bool PNG::Decode (Resource *resource, ImageBuffer *imageBuffer, bool decodeData, ImageDecodeScale *scale) {

		FILE_HANDLE* file = NULL;
		Bytes* data = NULL;

		if (resource->path) {

			file = lime::fopen (resource->path, "rb");
			if (!file) return false;
		}

		spng_ctx *ctx = spng_ctx_new (0);
		if (!ctx) {
			if (scale) scale->allocationFailed = true;
			if (file) lime::fclose (file);
			return false;
		}

		int result = SPNG_OK;

		if (file) {

			if (file->isFile ()) {

				result = spng_set_png_file (ctx, file->getFile ());

			} else if (scale) {

				result = spng_set_png_stream (ctx, ReadPngStream, file);

			} else {

				data = new Bytes ();
				data->ReadFile (resource->path);
				result = spng_set_png_buffer (ctx, data->b, data->length);

			}

		} else {

			result = spng_set_png_buffer (ctx, resource->data->b, resource->data->length);

		}

		if (result != SPNG_OK) {
			if (scale && result == SPNG_EMEM) scale->allocationFailed = true;
			spng_ctx_free (ctx);
			if (file) lime::fclose (file);
			if (data) delete data;
			return false;
		}

		struct spng_ihdr ihdr;
		result = spng_get_ihdr (ctx, &ihdr);

		if (result != SPNG_OK) {
			if (scale && result == SPNG_EMEM) scale->allocationFailed = true;
			spng_ctx_free (ctx);
			if (file) lime::fclose (file);
			if (data) delete data;
			return false;
		}

		if (scale) {

			scale->pngRecognized = true;
			bool decoded = decodeData && ihdr.interlace_method == 0 && scale->SetTarget (ihdr.width, ihdr.height)
				&& DecodeScaled (ctx, imageBuffer, *scale);
			spng_ctx_free (ctx);
			if (file) lime::fclose (file);
			if (data) delete data;
			return decoded;

		}

		if (decodeData) {
			size_t imageSize = 0;
			result = spng_decoded_image_size (ctx, SPNG_FMT_RGBA8, &imageSize);

			if (result != SPNG_OK) {
				spng_ctx_free (ctx);
				if (file) lime::fclose (file);
				if (data) delete data;
				return false;
			}

			imageBuffer->Resize (ihdr.width, ihdr.height, 32);

			unsigned char *bytes = imageBuffer->data->buffer->b;
			result = spng_decode_image (ctx, bytes, imageSize, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);

			if (result != SPNG_OK) {
				spng_ctx_free (ctx);
				if (file) lime::fclose (file);
				if (data) delete data;
				return false;
			}

			imageBuffer->format = BGRA32;
			imageBuffer->premultiplied = true;

			bool transparent = false;

			for (size_t i = 0; i + 3 < imageSize; i += 4) {
				unsigned int r = bytes[i];
				unsigned int g = bytes[i + 1];
				unsigned int b = bytes[i + 2];
				unsigned int a = bytes[i + 3];

				if (a != 255) {
					transparent = true;
					r = (r * a + 127) / 255;
					g = (g * a + 127) / 255;
					b = (b * a + 127) / 255;
				}

				bytes[i] = (unsigned char)b;
				bytes[i + 1] = (unsigned char)g;
				bytes[i + 2] = (unsigned char)r;
			}

			imageBuffer->transparent = transparent;

		} else {

			imageBuffer->width = ihdr.width;
			imageBuffer->height = ihdr.height;

		}

		spng_ctx_free (ctx);
		if (file) lime::fclose (file);
		if (data) delete data;

		return true;

	}


	bool PNG::Encode (ImageBuffer *imageBuffer, Bytes* bytes) {
		if (!imageBuffer || !bytes) return false;
		if (imageBuffer->bitsPerPixel != 32) return false;

		spng_ctx *ctx = spng_ctx_new (SPNG_CTX_ENCODER);
		if (!ctx) return false;

		int result = spng_set_option (ctx, SPNG_ENCODE_TO_BUFFER, 1);
		if (result != SPNG_OK) {
			spng_ctx_free (ctx);
			return false;
		}

		struct spng_ihdr ihdr = {};
		ihdr.width = imageBuffer->width;
		ihdr.height = imageBuffer->height;
		ihdr.bit_depth = 8;
		ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
		ihdr.compression_method = 0;
		ihdr.filter_method = 0;
		ihdr.interlace_method = 0;

		result = spng_set_ihdr (ctx, &ihdr);
		if (result != SPNG_OK) {
			spng_ctx_free (ctx);
			return false;
		}

		size_t len = (size_t)imageBuffer->Stride () * (size_t)imageBuffer->height;
		unsigned char* imageData = imageBuffer->data->buffer->b;
		unsigned char* tempData = NULL;

		if (imageBuffer->format != RGBA32 || imageBuffer->premultiplied) {
			tempData = (unsigned char*)malloc (len);
			if (!tempData) {
				spng_ctx_free (ctx);
				return false;
			}

			const unsigned char* src = imageData;
			unsigned char* dest = tempData;
			const unsigned char* const end = src + len;

			while (src < end) {
				unsigned int r = 0, g = 0, b = 0, a = 0;

				switch (imageBuffer->format) {
					case BGRA32:
						b = src[0]; g = src[1]; r = src[2]; a = src[3];
						break;
					case ARGB32:
						a = src[0]; r = src[1]; g = src[2]; b = src[3];
						break;
					case RGBA32:
					default:
						r = src[0]; g = src[1]; b = src[2]; a = src[3];
						break;
				}

				if (imageBuffer->premultiplied && a != 0 && a != 255) {
					r = (r * 255 + (a / 2)) / a;
					g = (g * 255 + (a / 2)) / a;
					b = (b * 255 + (a / 2)) / a;
					if (r > 255) r = 255;
					if (g > 255) g = 255;
					if (b > 255) b = 255;
				}

				dest[0] = (unsigned char)r;
				dest[1] = (unsigned char)g;
				dest[2] = (unsigned char)b;
				dest[3] = (unsigned char)a;

				src += 4;
				dest += 4;
			}

			imageData = tempData;
		}

		result = spng_encode_image (ctx, imageData, len, SPNG_FMT_RGBA8, SPNG_ENCODE_FINALIZE);
		if (result != SPNG_OK) {
			if (tempData) free (tempData);
			spng_ctx_free (ctx);
			return false;
		}

		size_t pngSize = 0;
		int encodeError = SPNG_OK;
		void *pngData = spng_get_png_buffer (ctx, &pngSize, &encodeError);

		if (!pngData || encodeError != SPNG_OK) {
			spng_ctx_free (ctx);
			return false;
		}

		bytes->Resize ((int)pngSize);
		memcpy (bytes->b, pngData, pngSize);

		if (tempData) free (tempData);
		free (pngData);
		spng_ctx_free (ctx);

		return true;

	}


}
