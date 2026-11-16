// Proof of concept for the Helios integration: measures PyroWave encode time, frame size,
// decode time and robustness to packet loss using real desktop capture (DXGI Desktop Duplication),
// D3D11 -> Vulkan interop and the public C API only. Windows only.
//
// Usage: pyrowave-poc-latency [--frames N] [--fps F] [--packet BYTES] [--res WxH]... [--mbps M]...
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <dxgi1_5.h>
#include <d3d11_4.h>
#include "volk.h"
#include "pyrowave.h"
#include "com_ptr.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#define ASSERT_THAT(x) do { \
	if (!(x)) { fprintf(stderr, "Fatal error executing %s at line %d.\n", #x, __LINE__); std::exit(1); } \
} while (false)
#define CHECK_HRESULT(x) ASSERT_THAT(SUCCEEDED(x))
#define CHECKED(x) do { \
	pyrowave_result res_ = (x); \
	if (res_ != PYROWAVE_SUCCESS) { fprintf(stderr, "pyrowave result %d executing %s at line %d.\n", (int)res_, #x, __LINE__); std::exit(1); } \
} while (false)

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Res { uint32_t w, h; };

struct Stats
{
	std::vector<double> v;
	void add(double x) { v.push_back(x); }
	double avg() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
	double pct(double p) const
	{
		if (v.empty()) return 0;
		std::vector<double> c = v;
		std::sort(c.begin(), c.end());
		return c[std::min(c.size() - 1, size_t(p * (c.size() - 1) + 0.5))];
	}
	double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

struct Planes
{
	std::vector<uint8_t> y, cb, cr;
	pyrowave_cpu_buffer buf;
	Planes(uint32_t w, uint32_t h) : y(w * h), cb(w * h / 4), cr(w * h / 4)
	{
		buf = {};
		buf.data[0] = y.data(); buf.data[1] = cb.data(); buf.data[2] = cr.data();
		buf.width = int(w); buf.height = int(h);
		buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
		for (int i = 0; i < 3; i++)
		{
			buf.plane_size_in_bytes[i] = (w * h) >> (i ? 2 : 0);
			buf.row_stride_in_bytes[i] = w >> (i ? 1 : 0);
		}
	}
};

static double psnr_luma(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
	double mse = 0;
	for (size_t i = 0; i < a.size(); i++) { double d = double(a[i]) - double(b[i]); mse += d * d; }
	mse /= double(a.size());
	return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

int main(int argc, char **argv)
{
	uint32_t frames = 120;
	double fps = 60.0;
	size_t packet_size = 1400;
	std::vector<Res> resolutions;
	std::vector<double> mbps_list;

	for (int i = 1; i < argc; i++)
	{
		auto next = [&]() -> const char * { ASSERT_THAT(i + 1 < argc); return argv[++i]; };
		if (!strcmp(argv[i], "--frames")) frames = uint32_t(atoi(next()));
		else if (!strcmp(argv[i], "--fps")) fps = atof(next());
		else if (!strcmp(argv[i], "--packet")) packet_size = size_t(atoi(next()));
		else if (!strcmp(argv[i], "--res")) { Res r{}; ASSERT_THAT(sscanf(next(), "%ux%u", &r.w, &r.h) == 2); resolutions.push_back(r); }
		else if (!strcmp(argv[i], "--mbps")) mbps_list.push_back(atof(next()));
		else { fprintf(stderr, "Unknown argument %s\n", argv[i]); return 1; }
	}
	if (resolutions.empty()) resolutions = { { 1920, 1080 }, { 2560, 1440 } };
	if (mbps_list.empty()) mbps_list = { 100, 200, 400 };

	// D3D11 device + desktop duplication.
	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11DeviceContext4> context4;
	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(0, (IDXGIAdapter **)adapter.ppv()));
	CHECK_HRESULT(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				(ID3D11Device **)device.ppv(), nullptr, (ID3D11DeviceContext **)context.ppv()));
	CHECK_HRESULT(device->QueryInterface(IID_ID3D11Device5, device5.ppv()));
	CHECK_HRESULT(context->QueryInterface(IID_ID3D11DeviceContext4, context4.ppv()));

	DXGI_ADAPTER_DESC adapter_desc;
	adapter->GetDesc(&adapter_desc);
	printf("Adapter LUID-matched Vulkan device for: %ls\n", adapter_desc.Description);

	ComPtr<IDXGIOutput> output;
	ComPtr<IDXGIOutput1> output1;
	ComPtr<IDXGIOutputDuplication> dupl;
	CHECK_HRESULT(adapter->EnumOutputs(0, (IDXGIOutput **)output.ppv()));
	CHECK_HRESULT(output->QueryInterface(IID_IDXGIOutput1, output1.ppv()));
	CHECK_HRESULT(output1->DuplicateOutput(device.get(), (IDXGIOutputDuplication **)dupl.ppv()));
	DXGI_OUTDUPL_DESC dd;
	dupl->GetDesc(&dd);
	const uint32_t desk_w = dd.ModeDesc.Width, desk_h = dd.ModeDesc.Height;
	printf("Desktop: %u x %u\n", desk_w, desk_h);

	// Our own shareable texture: every captured frame is copied into it, so the Vulkan import is done once.
	ComPtr<ID3D11Texture2D> shared_tex;
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = desk_w; td.Height = desk_h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		CHECK_HRESULT(device5->CreateTexture2D(&td, nullptr, (ID3D11Texture2D **)shared_tex.ppv()));
	}

	ComPtr<ID3D11Fence> share_fence;
	CHECK_HRESULT(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, share_fence.ppv()));
	HANDLE fence_handle;
	CHECK_HRESULT(share_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle));

	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_device_by_compat(0, 0, 0, 0,
				reinterpret_cast<const pyrowave_luid *>(&adapter_desc.AdapterLuid), VK_QUEUE_GLOBAL_PRIORITY_MEDIUM, &pyro_device));

	pyrowave_image pyro_image;
	{
		ComPtr<IDXGIResource1> res1;
		CHECK_HRESULT(shared_tex->QueryInterface(IID_IDXGIResource1, res1.ppv()));
		HANDLE h;
		CHECK_HRESULT(res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h));

		VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.extent = { desk_w, desk_h, 1 };
		ici.format = VK_FORMAT_B8G8R8A8_UNORM;
		ici.mipLevels = 1; ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		pyrowave_image_create_info info = {};
		info.device = pyro_device;
		info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
		info.image_create_info = &ici;
		info.external_handle = (pyrowave_os_handle)h;
		CHECKED(pyrowave_image_create(&info, &pyro_image));
	}

	pyrowave_sync_object pyro_sync;
	{
		pyrowave_sync_object_create_info si = {};
		si.device = pyro_device;
		si.external_handle = (pyrowave_os_handle)fence_handle;
		si.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
		si.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
		CHECKED(pyrowave_sync_object_create(&si, &pyro_sync));
	}

	uint64_t timeline = 0;
	bool have_frame = false;
	uint32_t distinct_frames = 0;

	// Grabs a new desktop frame if one is available (non-blocking) and copies it into the shared texture.
	auto capture = [&](double &capture_ms) {
		auto t = Clock::now();
		DXGI_OUTDUPL_FRAME_INFO fi = {};
		ComPtr<IDXGIResource> r;
		HRESULT hr = dupl->AcquireNextFrame(have_frame ? 0 : 1000, &fi, (IDXGIResource **)r.ppv());
		if (hr == S_OK)
		{
			ComPtr<ID3D11Texture2D> t2d;
			CHECK_HRESULT(r->QueryInterface(IID_ID3D11Texture2D, t2d.ppv()));
			context->CopyResource(shared_tex.get(), t2d.get());
			dupl->ReleaseFrame();
			have_frame = true;
			distinct_frames++;
		}
		else if (hr != DXGI_ERROR_WAIT_TIMEOUT)
		{
			fprintf(stderr, "AcquireNextFrame failed: 0x%x\n", (unsigned)hr);
			std::exit(1);
		}
		ASSERT_THAT(have_frame);
		capture_ms = ms_since(t);
	};

	printf("\nframes=%u fps=%.0f packet=%zu bytes; encode time = GPU encode + bitstream readback; "
	       "decode time = GPU decode + CPU readback (overestimates a display-only decode)\n\n", frames, fps, packet_size);
	printf("%-10s %6s | %8s %8s %8s | %9s %8s %7s | %8s\n", "res", "target", "enc avg", "enc p95", "enc max",
	       "KB/frame", "Mbit/s", "pkts", "dec avg");

	for (const Res &res : resolutions)
	{
		pyrowave_encoder encoder;
		pyrowave_decoder decoder;
		pyrowave_encoder_create_info eci = {};
		eci.device = pyro_device; eci.width = int(res.w); eci.height = int(res.h);
		eci.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
		CHECKED(pyrowave_encoder_create(&eci, &encoder));
		pyrowave_decoder_create_info dci = {};
		dci.device = pyro_device; dci.width = int(res.w); dci.height = int(res.h);
		dci.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
		CHECKED(pyrowave_decoder_create(&dci, &decoder));
		Planes out(res.w, res.h), ref(res.w, res.h);

		for (double mbps : mbps_list)
		{
			const size_t budget = size_t(mbps * 1e6 / 8.0 / fps);
			Stats enc_ms, dec_ms, size_kb, npkts;
			std::vector<uint8_t> last_stream;
			std::vector<pyrowave_packet> last_packets;

			for (uint32_t f = 0; f < frames + 10; f++)
			{
				double cap_ms;
				capture(cap_ms);

				context4->Signal(share_fence.get(), ++timeline);
				pyrowave_gpu_external_reference ext = {};
				ext.image = pyro_image;
				ext.queue_family_index = VK_QUEUE_FAMILY_EXTERNAL;
				pyrowave_gpu_sync_operation acquire = {}, release = {};
				acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync);
				acquire.sync.value = timeline;
				acquire.num_images = 1;
				acquire.images = &ext;
				release.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync);
				release.sync.value = ++timeline;

				pyrowave_scaled_encode_info si = {};
				CHECKED(pyrowave_image_get_image_view(pyro_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &si.view));
				si.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				si.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				si.ycbcr_chroma_midpoint = 128.0f / 255.0f;
				si.intermediate_plane_format = VK_FORMAT_R8_UNORM;
				pyrowave_rate_control rc = { budget };

				auto t0 = Clock::now();
				CHECKED(pyrowave_encoder_encode_gpu_scaled(encoder, &acquire, &release, &si, &rc));
				size_t npk = 0;
				CHECKED(pyrowave_encoder_compute_num_packets(encoder, packet_size, &npk));
				std::vector<pyrowave_packet> packets(npk);
				std::vector<uint8_t> stream(budget + 4096);
				size_t out_pk = 0;
				CHECKED(pyrowave_encoder_packetize(encoder, packets.data(), packet_size, &out_pk, stream.data(), stream.size()));
				double enc = ms_since(t0);
				context4->Wait(share_fence.get(), timeline);
				packets.resize(out_pk);

				size_t total = 0;
				for (auto &p : packets) total += p.size;

				pyrowave_decoder_clear(decoder);
				auto t1 = Clock::now();
				for (auto &p : packets)
					CHECKED(pyrowave_decoder_push_packet(decoder, stream.data() + p.offset, p.size));
				ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));
				CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &out.buf));
				double dec = ms_since(t1);

				if (f >= 10) // warm-up
				{
					enc_ms.add(enc); dec_ms.add(dec);
					size_kb.add(double(total) / 1024.0); npkts.add(double(out_pk));
				}
				last_stream = std::move(stream);
				last_packets = std::move(packets);
				if (f + 1 == frames + 10) ref.y = out.y;
			}

			printf("%4ux%-5u %4.0fM | %7.3fms %7.3fms %7.3fms | %9.1f %8.1f %7.0f | %7.3fms\n", res.w, res.h, mbps,
			       enc_ms.avg(), enc_ms.pct(0.95), enc_ms.max(),
			       size_kb.avg(), size_kb.avg() * 1024.0 * 8.0 * fps / 1e6, npkts.avg(), dec_ms.avg());

			// Packet loss: drop random packets of the last encoded frame, decode what is left.
			std::mt19937 rng(1234);
			for (double loss : { 1.0, 2.0, 5.0, 10.0 })
			{
				Stats psnr;
				for (int trial = 0; trial < 20; trial++)
				{
					pyrowave_decoder_clear(decoder);
					for (auto &p : last_packets)
						if (std::uniform_real_distribution<double>(0, 100)(rng) >= loss)
							pyrowave_decoder_push_packet(decoder, last_stream.data() + p.offset, p.size);
					pyrowave_decoder_decode_is_ready(decoder, true);
					CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &out.buf));
					psnr.add(psnr_luma(ref.y, out.y));
				}
				printf("    loss %4.1f%%: luma PSNR vs lossless decode avg %.1f dB (min %.1f dB)\n",
				       loss, psnr.avg(), psnr.v.empty() ? 0.0 : *std::min_element(psnr.v.begin(), psnr.v.end()));
			}
		}

		pyrowave_encoder_destroy(encoder);
		pyrowave_decoder_destroy(decoder);
	}

	printf("\nDistinct desktop frames captured: %u (static desktop = repeats of the last frame; use a game or video for realistic sizes)\n",
	       distinct_frames);
	pyrowave_image_destroy(pyro_image);
	pyrowave_sync_object_destroy(pyro_sync);
	pyrowave_device_destroy(pyro_device);
	return 0;
}
