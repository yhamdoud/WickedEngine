#include "wiHelper.h"
#include "wiPlatform.h"
#include "wiBacklog.h"
#include "wiEventHandler.h"
#include "wiMath.h"

#include "Utility/stb_image_write.h"
#include "Utility/basis_universal/encoder/basisu_comp.h"
#include "Utility/basis_universal/encoder/basisu_gpu_texture.h"
extern basist::etc1_global_selector_codebook g_basis_global_codebook;

#include <thread>
#include <locale>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <codecvt> // string conversion
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#ifdef PLATFORM_UWP
#include <winrt/Windows.UI.Popups.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#else
#include <Commdlg.h> // openfile
#include <WinBase.h>
#endif // PLATFORM_UWP
#else
#include "Utility/portable-file-dialogs.h"
#endif // _WIN32


namespace wi::helper
{

	std::string toUpper(const std::string& s)
	{
		std::string result;
		std::locale loc;
		for (unsigned int i = 0; i < s.length(); ++i)
		{
			result += std::toupper(s.at(i), loc);
		}
		return result;
	}

	void messageBox(const std::string& msg, const std::string& caption)
	{
#ifdef _WIN32
#ifndef PLATFORM_UWP
		MessageBoxA(GetActiveWindow(), msg.c_str(), caption.c_str(), 0);
#else
		std::wstring wmessage, wcaption;
		StringConvert(msg, wmessage);
		StringConvert(caption, wcaption);
		// UWP can only show message box on main thread:
		wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
			winrt::Windows::UI::Popups::MessageDialog(wmessage, wcaption).ShowAsync();
		});
#endif // PLATFORM_UWP
#elif SDL2
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, caption.c_str(), msg.c_str(), NULL);
#endif // _WIN32
	}

	void screenshot(const wi::graphics::SwapChain& swapchain, const std::string& name)
	{
		std::string directory;
		if (name.empty())
		{
			directory = std::filesystem::current_path().string() + "/screenshots";
		}
		else
		{
			directory = GetDirectoryFromPath(name);
		}

		DirectoryCreate(directory);

		std::string filename = name;
		if (filename.empty())
		{
			filename = directory + "/sc_" + getCurrentDateTimeAsString() + ".jpg";
		}

		bool result = saveTextureToFile(wi::graphics::GetDevice()->GetBackBuffer(&swapchain), filename);
		assert(result);

		if (result)
		{
			wi::backlog::post("Screenshot saved: " + filename);
		}
	}

	bool saveTextureToMemory(const wi::graphics::Texture& texture, wi::vector<uint8_t>& texturedata)
	{
		using namespace wi::graphics;

		GraphicsDevice* device = wi::graphics::GetDevice();

		TextureDesc desc = texture.GetDesc();

		Texture stagingTex;
		TextureDesc staging_desc = desc;
		staging_desc.usage = Usage::READBACK;
		staging_desc.mip_levels = 1;
		staging_desc.layout = ResourceState::COPY_DST;
		staging_desc.bind_flags = BindFlag::NONE;
		staging_desc.misc_flags = ResourceMiscFlag::NONE;
		bool success = device->CreateTexture(&staging_desc, nullptr, &stagingTex);
		assert(success);

		CommandList cmd = device->BeginCommandList();

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&texture,texture.desc.layout,ResourceState::COPY_SRC),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->CopyResource(&stagingTex, &texture, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&texture,ResourceState::COPY_SRC,texture.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->SubmitCommandLists();
		device->WaitForGPU();

		desc.width /= GetFormatBlockSize(desc.format);
		desc.height /= GetFormatBlockSize(desc.format);
		uint32_t data_count = desc.width * desc.height;
		uint32_t data_stride = GetFormatStride(desc.format);
		uint32_t data_size = data_count * data_stride;

		texturedata.clear();
		texturedata.resize(data_size);

		if (stagingTex.mapped_data != nullptr)
		{
			if (stagingTex.mapped_rowpitch / data_stride != desc.width)
			{
				// Copy padded texture row by row:
				const uint32_t cpysize = desc.width * data_stride;
				for (uint32_t i = 0; i < desc.height; ++i)
				{
					void* src = (void*)((size_t)stagingTex.mapped_data + size_t(i * stagingTex.mapped_rowpitch));
					void* dst = (void*)((size_t)texturedata.data() + size_t(i * cpysize));
					memcpy(dst, src, cpysize);
				}
			}
			else
			{
				// Copy whole
				std::memcpy(texturedata.data(), stagingTex.mapped_data, texturedata.size());
			}
		}
		else
		{
			assert(0);
		}

		return stagingTex.mapped_data != nullptr;
	}

	bool saveTextureToMemoryFile(const wi::graphics::Texture& texture, const std::string& fileExtension, wi::vector<uint8_t>& filedata)
	{
		using namespace wi::graphics;
		TextureDesc desc = texture.GetDesc();
		wi::vector<uint8_t> texturedata;
		if (saveTextureToMemory(texture, texturedata))
		{
			return saveTextureToMemoryFile(texturedata, desc, fileExtension, filedata);
		}
		return false;
	}

	bool saveTextureToMemoryFile(const wi::vector<uint8_t>& texturedata, const wi::graphics::TextureDesc& desc, const std::string& fileExtension, wi::vector<uint8_t>& filedata)
	{
		using namespace wi::graphics;
		uint32_t data_count = desc.width * desc.height;

		std::string extension = wi::helper::toUpper(fileExtension);
		bool basis = !extension.compare("BASIS");
		bool ktx2 = !extension.compare("KTX2");
		basisu::image basis_image;

		if (desc.format == Format::R10G10B10A2_UNORM)
		{
			// This will be converted first to rgba8 before saving to common format:
			uint32_t* data32 = (uint32_t*)texturedata.data();

			for (uint32_t i = 0; i < data_count; ++i)
			{
				uint32_t pixel = data32[i];
				float r = ((pixel >> 0) & 1023) / 1023.0f;
				float g = ((pixel >> 10) & 1023) / 1023.0f;
				float b = ((pixel >> 20) & 1023) / 1023.0f;
				float a = ((pixel >> 30) & 3) / 3.0f;

				uint32_t rgba8 = 0;
				rgba8 |= (uint32_t)(r * 255.0f) << 0;
				rgba8 |= (uint32_t)(g * 255.0f) << 8;
				rgba8 |= (uint32_t)(b * 255.0f) << 16;
				rgba8 |= (uint32_t)(a * 255.0f) << 24;

				data32[i] = rgba8;
			}
		}
		else if (desc.format == Format::R32G32B32A32_FLOAT)
		{
			// This will be converted first to rgba8 before saving to common format:
			XMFLOAT4* dataSrc = (XMFLOAT4*)texturedata.data();
			uint32_t* data32 = (uint32_t*)texturedata.data();

			for (uint32_t i = 0; i < data_count; ++i)
			{
				XMFLOAT4 pixel = dataSrc[i];
				float r = std::max(0.0f, std::min(pixel.x, 1.0f));
				float g = std::max(0.0f, std::min(pixel.y, 1.0f));
				float b = std::max(0.0f, std::min(pixel.z, 1.0f));
				float a = std::max(0.0f, std::min(pixel.w, 1.0f));

				uint32_t rgba8 = 0;
				rgba8 |= (uint32_t)(r * 255.0f) << 0;
				rgba8 |= (uint32_t)(g * 255.0f) << 8;
				rgba8 |= (uint32_t)(b * 255.0f) << 16;
				rgba8 |= (uint32_t)(a * 255.0f) << 24;

				data32[i] = rgba8;
			}
		}
		else if (desc.format == Format::R16G16B16A16_FLOAT)
		{
			// This will be converted first to rgba8 before saving to common format:
			XMHALF4* dataSrc = (XMHALF4*)texturedata.data();
			uint32_t* data32 = (uint32_t*)texturedata.data();

			for (uint32_t i = 0; i < data_count; ++i)
			{
				XMHALF4 pixel = dataSrc[i];
				float r = std::max(0.0f, std::min(XMConvertHalfToFloat(pixel.x), 1.0f));
				float g = std::max(0.0f, std::min(XMConvertHalfToFloat(pixel.y), 1.0f));
				float b = std::max(0.0f, std::min(XMConvertHalfToFloat(pixel.z), 1.0f));
				float a = std::max(0.0f, std::min(XMConvertHalfToFloat(pixel.w), 1.0f));

				uint32_t rgba8 = 0;
				rgba8 |= (uint32_t)(r * 255.0f) << 0;
				rgba8 |= (uint32_t)(g * 255.0f) << 8;
				rgba8 |= (uint32_t)(b * 255.0f) << 16;
				rgba8 |= (uint32_t)(a * 255.0f) << 24;

				data32[i] = rgba8;
			}
		}
		else if (desc.format == Format::R11G11B10_FLOAT)
		{
			// This will be converted first to rgba8 before saving to common format:
			XMFLOAT3PK* dataSrc = (XMFLOAT3PK*)texturedata.data();
			uint32_t* data32 = (uint32_t*)texturedata.data();

			for (uint32_t i = 0; i < data_count; ++i)
			{
				XMFLOAT3PK pixel = dataSrc[i];
				XMVECTOR V = XMLoadFloat3PK(&pixel);
				XMFLOAT3 pixel3;
				XMStoreFloat3(&pixel3, V);
				float r = std::max(0.0f, std::min(pixel3.x, 1.0f));
				float g = std::max(0.0f, std::min(pixel3.y, 1.0f));
				float b = std::max(0.0f, std::min(pixel3.z, 1.0f));
				float a = 1;

				uint32_t rgba8 = 0;
				rgba8 |= (uint32_t)(r * 255.0f) << 0;
				rgba8 |= (uint32_t)(g * 255.0f) << 8;
				rgba8 |= (uint32_t)(b * 255.0f) << 16;
				rgba8 |= (uint32_t)(a * 255.0f) << 24;

				data32[i] = rgba8;
			}
		}
		else if (IsFormatBlockCompressed(desc.format))
		{
			basisu::texture_format fmt;
			switch (desc.format)
			{
			default:
				assert(0);
				return false;
			case Format::BC1_UNORM:
			case Format::BC1_UNORM_SRGB:
				fmt = basisu::texture_format::cBC1;
				break;
			case Format::BC3_UNORM:
			case Format::BC3_UNORM_SRGB:
				fmt = basisu::texture_format::cBC3;
				break;
			case Format::BC4_UNORM:
				fmt = basisu::texture_format::cBC4;
				break;
			case Format::BC5_UNORM:
				fmt = basisu::texture_format::cBC5;
				break;
			case Format::BC7_UNORM:
			case Format::BC7_UNORM_SRGB:
				fmt = basisu::texture_format::cBC7;
				break;
			}
			basisu::gpu_image basis_gpu_image;
			basis_gpu_image.init(fmt, desc.width, desc.height);
			std::memcpy(basis_gpu_image.get_ptr(), texturedata.data(), std::min(texturedata.size(), (size_t)basis_gpu_image.get_size_in_bytes()));
			basis_gpu_image.unpack(basis_image);
		}
		else
		{
			assert(desc.format == Format::R8G8B8A8_UNORM); // If you need to save other texture format, implement data conversion for it
		}

		if (basis || ktx2)
		{
			if (basis_image.get_total_pixels() == 0)
			{
				basis_image.init(texturedata.data(), desc.width, desc.height, 4);
			}
			basisu::basis_compressor_params params;
			params.m_source_images.push_back(basis_image);
			if (ktx2)
			{
				params.m_create_ktx2_file = true;
			}
			else
			{
				params.m_create_ktx2_file = false;
			}
#if 1
			params.m_compression_level = basisu::BASISU_DEFAULT_COMPRESSION_LEVEL;
#else
			params.m_compression_level = basisu::BASISU_MAX_COMPRESSION_LEVEL;
#endif
			params.m_mip_gen = true;
			params.m_pSel_codebook = &g_basis_global_codebook;
			params.m_quality_level = basisu::BASISU_QUALITY_MAX;
			params.m_multithreading = true;
			int num_threads = std::max(1u, std::thread::hardware_concurrency());
			basisu::job_pool jpool(num_threads);
			params.m_pJob_pool = &jpool;
			basisu::basis_compressor compressor;
			if (compressor.init(params))
			{
				auto result = compressor.process();
				if (result == basisu::basis_compressor::cECSuccess)
				{
					if (basis)
					{
						const auto& basis_file = compressor.get_output_basis_file();
						filedata.resize(basis_file.size());
						std::memcpy(filedata.data(), basis_file.data(), basis_file.size());
						return true;
					}
					else if (ktx2)
					{
						const auto& ktx2_file = compressor.get_output_ktx2_file();
						filedata.resize(ktx2_file.size());
						std::memcpy(filedata.data(), ktx2_file.data(), ktx2_file.size());
						return true;
					}
				}
			}
			return false;
		}

		int write_result = 0;

		filedata.clear();
		stbi_write_func* func = [](void* context, void* data, int size) {
			wi::vector<uint8_t>& filedata = *(wi::vector<uint8_t>*)context;
			for (int i = 0; i < size; ++i)
			{
				filedata.push_back(*((uint8_t*)data + i));
			}
		};

		const void* src_data = texturedata.data();
		if (basis_image.get_width() > 0 && basis_image.get_ptr() != nullptr)
		{
			src_data = basis_image.get_ptr();
		}

		if (!extension.compare("JPG") || !extension.compare("JPEG"))
		{
			write_result = stbi_write_jpg_to_func(func, &filedata, (int)desc.width, (int)desc.height, 4, src_data, 100);
		}
		else if (!extension.compare("PNG"))
		{
			write_result = stbi_write_png_to_func(func, &filedata, (int)desc.width, (int)desc.height, 4, src_data, 0);
		}
		else if (!extension.compare("TGA"))
		{
			write_result = stbi_write_tga_to_func(func, &filedata, (int)desc.width, (int)desc.height, 4, src_data);
		}
		else if (!extension.compare("BMP"))
		{
			write_result = stbi_write_bmp_to_func(func, &filedata, (int)desc.width, (int)desc.height, 4, src_data);
		}
		else
		{
			assert(0 && "Unsupported extension");
		}

		return write_result != 0;
	}

	bool saveTextureToFile(const wi::graphics::Texture& texture, const std::string& fileName)
	{
		using namespace wi::graphics;
		TextureDesc desc = texture.GetDesc();
		wi::vector<uint8_t> data;
		if (saveTextureToMemory(texture, data))
		{
			return saveTextureToFile(data, desc, fileName);
		}
		return false;
	}

	bool saveTextureToFile(const wi::vector<uint8_t>& texturedata, const wi::graphics::TextureDesc& desc, const std::string& fileName)
	{
		using namespace wi::graphics;

		std::string ext = GetExtensionFromFileName(fileName);
		wi::vector<uint8_t> filedata;
		if (saveTextureToMemoryFile(texturedata, desc, ext, filedata))
		{
			return FileWrite(fileName, filedata.data(), filedata.size());
		}

		return false;
	}

	std::string getCurrentDateTimeAsString()
	{
		time_t t = std::time(nullptr);
		struct tm time_info;
#ifdef _WIN32
		localtime_s(&time_info, &t);
#else
		localtime(&t);
#endif
		std::stringstream ss("");
		ss << std::put_time(&time_info, "%d-%m-%Y %H-%M-%S");
		return ss.str();
	}

	void SplitPath(const std::string& fullPath, std::string& dir, std::string& fileName)
	{
		size_t found;
		found = fullPath.find_last_of("/\\");
		dir = fullPath.substr(0, found + 1);
		fileName = fullPath.substr(found + 1);
	}

	std::string GetFileNameFromPath(const std::string& fullPath)
	{
		if (fullPath.empty())
		{
			return fullPath;
		}

		std::string ret, empty;
		SplitPath(fullPath, empty, ret);
		return ret;
	}

	std::string GetDirectoryFromPath(const std::string& fullPath)
	{
		if (fullPath.empty())
		{
			return fullPath;
		}

		std::string ret, empty;
		SplitPath(fullPath, ret, empty);
		return ret;
	}

	std::string GetExtensionFromFileName(const std::string& filename)
	{
		size_t idx = filename.rfind('.');

		if (idx != std::string::npos)
		{
			std::string extension = filename.substr(idx + 1);
			return extension;
		}

		// No extension found
		return "";
	}

	std::string ReplaceExtension(const std::string& filename, const std::string& extension)
	{
		size_t idx = filename.rfind('.');

		if (idx == std::string::npos)
		{
			// extension not found, append it:
			return filename + "." + extension;
		}
		return filename.substr(0, idx + 1) + extension;
	}

	std::string RemoveExtension(const std::string& filename)
	{
		size_t idx = filename.rfind('.');

		if (idx == std::string::npos)
		{
			// extension not found:
			return filename;
		}
		return filename.substr(0, idx);
	}

	void MakePathRelative(const std::string& rootdir, std::string& path)
	{
		if (rootdir.empty() || path.empty())
		{
			return;
		}

		std::filesystem::path filepath = path;
		std::filesystem::path rootpath = rootdir;
		std::filesystem::path relative = std::filesystem::relative(path, rootdir);
		if (!relative.empty())
		{
			path = relative.string();
		}

		//size_t found = path.rfind(rootdir);
		//if (found != std::string::npos)
		//{
		//	path = path.substr(found + rootdir.length());
		//}
	}

	void MakePathAbsolute(std::string& path)
	{
		std::filesystem::path filepath = path;
		std::filesystem::path absolute = std::filesystem::absolute(path);
		if (!absolute.empty())
		{
			path = absolute.string();
		}
	}

	void DirectoryCreate(const std::string& path)
	{
		std::filesystem::create_directories(path);
	}

	template<template<typename T, typename A> typename vector_interface>
	bool FileRead_Impl(const std::string& fileName, vector_interface<uint8_t, std::allocator<uint8_t>>& data)
	{
#ifndef PLATFORM_UWP
#ifdef SDL_FILESYSTEM_UNIX
		std::string filepath = fileName;
		std::replace(filepath.begin(), filepath.end(), '\\', '/');
		std::ifstream file(filepath, std::ios::binary | std::ios::ate);
#else
		std::ifstream file(fileName, std::ios::binary | std::ios::ate);
#endif // SDL_FILESYSTEM_UNIX
		if (file.is_open())
		{
			size_t dataSize = (size_t)file.tellg();
			file.seekg(0, file.beg);
			data.resize(dataSize);
			file.read((char*)data.data(), dataSize);
			file.close();
			return true;
		}
#else
		using namespace winrt::Windows::Storage;
		using namespace winrt::Windows::Storage::Streams;
		using namespace winrt::Windows::Foundation;
		std::wstring wstr;
		std::filesystem::path filepath = fileName;
		filepath = std::filesystem::absolute(filepath);
		StringConvert(filepath.string(), wstr);
		bool success = false;

		auto async_helper = [&]() -> IAsyncAction {
			try
			{
				auto file = co_await StorageFile::GetFileFromPathAsync(wstr);
				auto buffer = co_await FileIO::ReadBufferAsync(file);
				auto reader = DataReader::FromBuffer(buffer);
				auto size = buffer.Length();
				data.resize((size_t)size);
				for (auto& x : data)
				{
					x = reader.ReadByte();
				}
				success = true;
			}
			catch (winrt::hresult_error const& ex)
			{
				switch (ex.code())
				{
				case E_ACCESSDENIED:
					wi::backlog::post("Opening file failed: " + fileName + " | Reason: Permission Denied!");
					break;
				default:
					break;
				}
			}

		};

		if (winrt::impl::is_sta_thread())
		{
			std::thread([&] { async_helper().get(); }).join(); // can't block coroutine from ui thread
		}
		else
		{
			async_helper().get();
		}

		if (success)
		{
			return true;
		}

#endif // PLATFORM_UWP

		wi::backlog::post("File not found: " + fileName);
		return false;
	}
	bool FileRead(const std::string& fileName, wi::vector<uint8_t>& data)
	{
		return FileRead_Impl(fileName, data);
	}
#if WI_VECTOR_TYPE
	bool FileRead(const std::string& fileName, std::vector<uint8_t>& data)
	{
		return FileRead_Impl(fileName, data);
	}
#endif // WI_VECTOR_TYPE

	bool FileWrite(const std::string& fileName, const uint8_t* data, size_t size)
	{
		if (size <= 0)
		{
			return false;
		}

#ifndef PLATFORM_UWP
		std::ofstream file(fileName, std::ios::binary | std::ios::trunc);
		if (file.is_open())
		{
			file.write((const char*)data, (std::streamsize)size);
			file.close();
			return true;
		}
#else

		using namespace winrt::Windows::Storage;
		using namespace winrt::Windows::Storage::Streams;
		using namespace winrt::Windows::Foundation;
		std::wstring wstr;
		std::filesystem::path filepath = fileName;
		filepath = std::filesystem::absolute(filepath);
		StringConvert(filepath.string(), wstr);

		CREATEFILE2_EXTENDED_PARAMETERS params = {};
		params.dwSize = (DWORD)size;
		params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		HANDLE filehandle = CreateFile2FromAppW(wstr.c_str(), GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, &params);
		assert(filehandle);
		CloseHandle(filehandle);

		bool success = false;
		auto async_helper = [&]() -> IAsyncAction {
			try
			{
				auto file = co_await StorageFile::GetFileFromPathAsync(wstr);
				winrt::array_view<const uint8_t> dataarray(data, (winrt::array_view<const uint8_t>::size_type)size);
				co_await FileIO::WriteBytesAsync(file, dataarray);
				success = true;
			}
			catch (winrt::hresult_error const& ex)
			{
				switch (ex.code())
				{
				case E_ACCESSDENIED:
					wi::backlog::post("Opening file failed: " + fileName + " | Reason: Permission Denied!");
					break;
				default:
					break;
				}
			}

		};

		if (winrt::impl::is_sta_thread())
		{
			std::thread([&] { async_helper().get(); }).join(); // can't block coroutine from ui thread
		}
		else
		{
			async_helper().get();
		}

		if (success)
		{
			return true;
		}
#endif // PLATFORM_UWP

		return false;
	}

	bool FileExists(const std::string& fileName)
	{
#ifndef PLATFORM_UWP
		bool exists = std::filesystem::exists(fileName);
		return exists;
#else
		using namespace winrt::Windows::Storage;
		using namespace winrt::Windows::Storage::Streams;
		using namespace winrt::Windows::Foundation;
		std::wstring wstr;
		std::filesystem::path filepath = fileName;
		filepath = std::filesystem::absolute(filepath);
		StringConvert(filepath.string(), wstr);
		bool success = false;

		auto async_helper = [&]() -> IAsyncAction {
			try
			{
				auto file = co_await StorageFile::GetFileFromPathAsync(wstr);
				success = true;
			}
			catch (winrt::hresult_error const& ex)
			{
				switch (ex.code())
				{
				case E_ACCESSDENIED:
					wi::backlog::post("Opening file failed: " + fileName + " | Reason: Permission Denied!");
					break;
				default:
					break;
				}
			}

		};

		if (winrt::impl::is_sta_thread())
		{
			std::thread([&] { async_helper().get(); }).join(); // can't block coroutine from ui thread
		}
		else
		{
			async_helper().get();
		}

		if (success)
		{
			return true;
		}
#endif // PLATFORM_UWP
	}

	std::string GetTempDirectoryPath()
	{
		auto path = std::filesystem::temp_directory_path();
		return path.string();
	}

	std::string GetCurrentPath()
	{
		auto path = std::filesystem::current_path();
		return path.string();
	}

	void FileDialog(const FileDialogParams& params, std::function<void(std::string fileName)> onSuccess)
	{
#ifdef _WIN32
#ifndef PLATFORM_UWP

		std::thread([=] {

			char szFile[256];

			OPENFILENAMEA ofn;
			ZeroMemory(&ofn, sizeof(ofn));
			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = nullptr;
			ofn.lpstrFile = szFile;
			// Set lpstrFile[0] to '\0' so that GetOpenFileName does not 
			// use the contents of szFile to initialize itself.
			ofn.lpstrFile[0] = '\0';
			ofn.nMaxFile = sizeof(szFile);
			ofn.lpstrFileTitle = NULL;
			ofn.nMaxFileTitle = 0;
			ofn.lpstrInitialDir = NULL;
			ofn.nFilterIndex = 1;

			// Slightly convoluted way to create the filter.
			//	First string is description, ended by '\0'
			//	Second string is extensions, each separated by ';' and at the end of all, a '\0'
			//	Then the whole container string is closed with an other '\0'
			//		For example: "model files\0*.model;*.obj;\0"  <-- this string literal has "model files" as description and two accepted extensions "model" and "obj"
			wi::vector<char> filter;
			filter.reserve(256);
			{
				for (auto& x : params.description)
				{
					filter.push_back(x);
				}
				filter.push_back(0);

				for (auto& x : params.extensions)
				{
					filter.push_back('*');
					filter.push_back('.');
					for (auto& y : x)
					{
						filter.push_back(y);
					}
					filter.push_back(';');
				}
				filter.push_back(0);
				filter.push_back(0);
			}
			ofn.lpstrFilter = filter.data();


			BOOL ok = FALSE;
			switch (params.type)
			{
			case FileDialogParams::OPEN:
				ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
				ofn.Flags |= OFN_NOCHANGEDIR;
				ok = GetOpenFileNameA(&ofn) == TRUE;
				break;
			case FileDialogParams::SAVE:
				ofn.Flags = OFN_OVERWRITEPROMPT;
				ofn.Flags |= OFN_NOCHANGEDIR;
				ok = GetSaveFileNameA(&ofn) == TRUE;
				break;
			}

			if (ok)
			{
				onSuccess(ofn.lpstrFile);
			}

			}).detach();

#else
		auto filedialoghelper = [](FileDialogParams params, std::function<void(std::string fileName)> onSuccess) -> winrt::fire_and_forget {

			using namespace winrt::Windows::Storage;
			using namespace winrt::Windows::Storage::Pickers;
			using namespace winrt::Windows::Storage::AccessCache;

			switch (params.type)
			{
			default:
			case FileDialogParams::OPEN:
			{
				FileOpenPicker picker;
				picker.ViewMode(PickerViewMode::List);
				picker.SuggestedStartLocation(PickerLocationId::Objects3D);

				for (auto& x : params.extensions)
				{
					std::wstring wstr;
					StringConvert(x, wstr);
					wstr = L"." + wstr;
					picker.FileTypeFilter().Append(wstr);
				}

				auto file = co_await picker.PickSingleFileAsync();

				if (file)
				{
					auto futureaccess = StorageApplicationPermissions::FutureAccessList();
					futureaccess.Clear();
					futureaccess.Add(file);
					std::wstring wstr = file.Path().data();
					std::string str;
					StringConvert(wstr, str);

					onSuccess(str);
				}
			}
			break;
			case FileDialogParams::SAVE:
			{
				FileSavePicker picker;
				picker.SuggestedStartLocation(PickerLocationId::Objects3D);

				std::wstring wdesc;
				StringConvert(params.description, wdesc);
				winrt::Windows::Foundation::Collections::IVector<winrt::hstring> extensions{ winrt::single_threaded_vector<winrt::hstring>() };
				for (auto& x : params.extensions)
				{
					std::wstring wstr;
					StringConvert(x, wstr);
					wstr = L"." + wstr;
					extensions.Append(wstr);
				}
				picker.FileTypeChoices().Insert(wdesc, extensions);

				auto file = co_await picker.PickSaveFileAsync();
				if (file)
				{
					auto futureaccess = StorageApplicationPermissions::FutureAccessList();
					futureaccess.Clear();
					futureaccess.Add(file);
					std::wstring wstr = file.Path().data();
					std::string str;
					StringConvert(wstr, str);
					onSuccess(str);
				}
			}
			break;
			}
		};
		filedialoghelper(params, onSuccess);

#endif // PLATFORM_UWP

#else
		if (!pfd::settings::available()) {
			const char *message = "No dialog backend available";
#ifdef SDL2
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
									 "File dialog error!",
									 message,
									 nullptr);
#endif
			std::cerr << message << std::endl;
		}

		std::vector<std::string> extensions = {params.description, ""};
		for (auto& x : params.extensions)
		{
			extensions[1] += "*." + x + " ";
		}

		switch (params.type) {
			case FileDialogParams::OPEN: {
				std::vector<std::string> selection = pfd::open_file(
					"Open file",
					std::filesystem::current_path().string(),
					extensions
				).result();
				if (!selection.empty())
				{
					onSuccess(selection[0]);
				}
				break;
			}
			case FileDialogParams::SAVE: {
				std::string destination = pfd::save_file(
					"Save file",
					std::filesystem::current_path().string(),
					extensions
				).result();
				if (!destination.empty())
				{
					onSuccess(destination);
				}
				break;
			}
		}
#endif // _WIN32
	}

	bool Bin2H(const uint8_t* data, size_t size, const std::string& dst_filename, const char* dataName)
	{
		std::string ss;
		ss += "const uint8_t ";
		ss += dataName ;
		ss += "[] = {";
		for (size_t i = 0; i < size; ++i)
		{
			if (i % 32 == 0)
			{
				ss += "\n";
			}
			ss += std::to_string((uint32_t)data[i]) + ",";
		}
		ss += "\n};\n";
		return FileWrite(dst_filename, (uint8_t*)ss.c_str(), ss.length());
	}

	void StringConvert(const std::string& from, std::wstring& to)
	{
#ifdef _WIN32
		int num = MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, NULL, 0);
		if (num > 0)
		{
			to.resize(size_t(num) - 1);
			MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, &to[0], num);
		}
#else
		std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
		to = cv.from_bytes(from);
#endif // _WIN32
	}

	void StringConvert(const std::wstring& from, std::string& to)
	{
#ifdef _WIN32
		int num = WideCharToMultiByte(CP_UTF8, 0, from.c_str(), -1, NULL, 0, NULL, NULL);
		if (num > 0)
		{
			to.resize(size_t(num) - 1);
			WideCharToMultiByte(CP_UTF8, 0, from.c_str(), -1, &to[0], num, NULL, NULL);
		}
#else
		std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
		to = cv.to_bytes(from);
#endif // _WIN32
	}

	int StringConvert(const char* from, wchar_t* to)
	{
#ifdef _WIN32
		int num = MultiByteToWideChar(CP_UTF8, 0, from, -1, NULL, 0);
		if (num > 0)
		{
			MultiByteToWideChar(CP_UTF8, 0, from, -1, &to[0], num);
		}
		return num;
#else
		std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
		std::memcpy(to, cv.from_bytes(from).c_str(), cv.converted());
		return (int)cv.converted();
#endif // _WIN32
	}

	int StringConvert(const wchar_t* from, char* to)
	{
#ifdef _WIN32
		int num = WideCharToMultiByte(CP_UTF8, 0, from, -1, NULL, 0, NULL, NULL);
		if (num > 0)
		{
			WideCharToMultiByte(CP_UTF8, 0, from, -1, &to[0], num, NULL, NULL);
		}
		return num;
#else
		std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
		std::memcpy(to, cv.to_bytes(from).c_str(), cv.converted());
		return (int)cv.converted();
#endif // _WIN32
	}
	
	void Sleep(float milliseconds)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds((int)milliseconds));
	}

	void Spin(float milliseconds)
	{
		milliseconds /= 1000.0f;
		std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
		double ms = 0;
		while (ms < milliseconds)
		{
			std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
			ms = time_span.count();
		}
	}
}
