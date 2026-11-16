// Compares the compute and fragment (mobile) decode paths against the original image,
// using only the C API and CPU buffers. Runs on any platform with Vulkan, including Android.
//
// Usage: pyrowave-poc-decode-paths [WIDTH HEIGHT [BYTES_PER_PIXEL_X100]]
#include "volk.h"
#include "pyrowave.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECKED(x) do { \
	pyrowave_result res_ = (x); \
	if (res_ != PYROWAVE_SUCCESS) { fprintf(stderr, "pyrowave result %d executing %s at line %d\n", (int)res_, #x, __LINE__); return 1; } \
} while (false)

struct Image
{
	int w, h;
	std::vector<uint8_t> y, cb, cr;
	pyrowave_cpu_buffer buf;
	Image(int w_, int h_) : w(w_), h(h_), y(size_t(w_) * h_), cb(size_t(w_) * h_ / 4), cr(size_t(w_) * h_ / 4)
	{
		buf = {};
		buf.data[0] = y.data(); buf.data[1] = cb.data(); buf.data[2] = cr.data();
		buf.width = w; buf.height = h;
		buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
		for (int i = 0; i < 3; i++)
		{
			buf.plane_size_in_bytes[i] = (size_t(w) * h) >> (i ? 2 : 0);
			buf.row_stride_in_bytes[i] = size_t(w) >> (i ? 1 : 0);
		}
	}
};

struct Err { int max_err; double psnr; int first_x, first_y; };

static Err compare(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int stride)
{
	Err e = { 0, 99.0, -1, -1 };
	double mse = 0;
	for (size_t i = 0; i < a.size(); i++)
	{
		int d = std::abs(int(a[i]) - int(b[i]));
		mse += double(d) * d;
		if (d > e.max_err) { e.max_err = d; e.first_x = int(i % stride); e.first_y = int(i / stride); }
	}
	mse /= double(a.size());
	if (mse > 0) e.psnr = 10.0 * std::log10(255.0 * 255.0 / mse);
	return e;
}

int main(int argc, char **argv)
{
	int w = argc > 2 ? atoi(argv[1]) : 1920;
	int h = argc > 2 ? atoi(argv[2]) : 1080;
	size_t bpp100 = argc > 3 ? size_t(atoi(argv[3])) : 400; // 4 bytes per pixel: effectively unlimited
	w &= ~1; h &= ~1;

	// Synthetic content: smooth gradients, sharp edges (text-like), and mild noise.
	Image src(w, h);
	uint32_t seed = 12345;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 24) & 7; };
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			int v = (x * 255 / w + y * 128 / h) / 2 + int(rnd()) - 3;
			if (((x / 16) + (y / 16)) % 7 == 0) v = 255 - v;
			if (y % 64 < 2 || x % 64 < 2) v = 235;
			src.y[size_t(y) * w + x] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	for (int y = 0; y < h / 2; y++)
		for (int x = 0; x < w / 2; x++)
		{
			src.cb[size_t(y) * (w / 2) + x] = uint8_t(128 + 60 * std::sin(x * 0.02) );
			src.cr[size_t(y) * (w / 2) + x] = uint8_t(128 + 60 * std::cos(y * 0.03) );
		}

	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));
	bool prefers_fragment = pyrowave_decoder_device_prefers_fragment_path(device);
	printf("size %dx%d, budget %.2f bytes/pixel, device prefers fragment path: %s\n", w, h, bpp100 / 100.0, prefers_fragment ? "yes" : "no");

	pyrowave_encoder_create_info eci = {};
	eci.device = device; eci.width = w; eci.height = h; eci.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_encoder_create(&eci, &encoder));
	size_t budget = size_t(w) * h * bpp100 / 100;
	pyrowave_rate_control rc = { budget };
	CHECKED(pyrowave_encoder_encode_cpu(encoder, &src.buf, &rc));
	size_t npk = 0;
	CHECKED(pyrowave_encoder_compute_num_packets(encoder, budget + 4096, &npk));
	std::vector<pyrowave_packet> packets(npk);
	std::vector<uint8_t> stream(budget + 4096 + 4096);
	size_t out_pk = 0;
	CHECKED(pyrowave_encoder_packetize(encoder, packets.data(), budget + 4096, &out_pk, stream.data(), stream.size()));
	printf("encoded: %zu bytes in %zu packet(s)\n", packets[0].size, out_pk);

	if (argc > 4)
	{
		// Does pushing the whole packetized frame in one call equal pushing packet by packet?
		size_t boundary = size_t(atoi(argv[4]));
		CHECKED(pyrowave_encoder_compute_num_packets(encoder, boundary, &npk));
		std::vector<pyrowave_packet> small(npk);
		std::vector<uint8_t> sstream(budget + 4096 + 4096);
		size_t spk = 0;
		CHECKED(pyrowave_encoder_packetize(encoder, small.data(), boundary, &spk, sstream.data(), sstream.size()));
		size_t total = 0, expected_offset = 0;
		bool contiguous = true;
		for (size_t i = 0; i < spk; i++) { contiguous &= small[i].offset == expected_offset; expected_offset += small[i].size; total += small[i].size; }
		printf("packetized at %zu: %zu packets, %zu bytes, contiguous=%d\n", boundary, spk, total, contiguous ? 1 : 0);

		pyrowave_decoder_create_info dci = {};
		dci.device = device; dci.width = w; dci.height = h; dci.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
		dci.fragment_path = false;
		pyrowave_decoder dec_a, dec_b;
		CHECKED(pyrowave_decoder_create(&dci, &dec_a));
		CHECKED(pyrowave_decoder_create(&dci, &dec_b));
		for (size_t i = 0; i < spk; i++)
			CHECKED(pyrowave_decoder_push_packet(dec_a, sstream.data() + small[i].offset, small[i].size));
		pyrowave_result r = pyrowave_decoder_push_packet(dec_b, sstream.data(), total);
		printf("push whole frame in one call: result %d, ready: per-packet=%d whole=%d\n", (int)r,
		       pyrowave_decoder_decode_is_ready(dec_a, false) ? 1 : 0, pyrowave_decoder_decode_is_ready(dec_b, false) ? 1 : 0);
		Image oa(w, h), ob(w, h);
		CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(dec_a, &oa.buf));
		CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(dec_b, &ob.buf));
		printf("outputs identical: luma=%d cb=%d cr=%d\n", oa.y == ob.y, oa.cb == ob.cb, oa.cr == ob.cr);
		pyrowave_decoder_destroy(dec_a);
		pyrowave_decoder_destroy(dec_b);
	}

	for (int frag = 0; frag < 2; frag++)
	{
		pyrowave_decoder_create_info dci = {};
		dci.device = device; dci.width = w; dci.height = h;
		dci.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
		dci.fragment_path = frag != 0;
		pyrowave_decoder decoder;
		CHECKED(pyrowave_decoder_create(&dci, &decoder));
		CHECKED(pyrowave_decoder_push_packet(decoder, stream.data() + packets[0].offset, packets[0].size));
		if (!pyrowave_decoder_decode_is_ready(decoder, false)) { printf("%s: decode not ready\n", frag ? "fragment" : "compute "); return 1; }
		Image out(w, h);
		CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &out.buf));
		Err ey = compare(src.y, out.y, w), ecb = compare(src.cb, out.cb, w / 2), ecr = compare(src.cr, out.cr, w / 2);
		printf("%s: luma max err %3d (at %d,%d) PSNR %5.1f dB | cb max %3d PSNR %5.1f | cr max %3d PSNR %5.1f | out(0,0)=%d src(0,0)=%d\n",
		       frag ? "fragment" : "compute ", ey.max_err, ey.first_x, ey.first_y, ey.psnr, ecb.max_err, ecb.psnr, ecr.max_err, ecr.psnr,
		       out.y[0], src.y[0]);
		pyrowave_decoder_destroy(decoder);
	}
	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(device);
	return 0;
}
