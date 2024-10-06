#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "updater.hpp"
#include "game/game.hpp"

#include <utils/flags.hpp>
#include <updater/updater.hpp>
#include <steam/steam.hpp>

#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <knownfolders.h>
#include <shlobj_core.h>

namespace updater
{
	//boiii vanilla installation functions are below. boiii ezz would install through online instead, whereas this is all done manually.
	void install_desktopshortcut(const std::filesystem::path& targetExe, const std::string& shortcutName, const std::filesystem::path& workingDirectory) {
		CoInitialize(nullptr);
		IShellLink* pShellLink = nullptr;
		HRESULT hres = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, reinterpret_cast<void**>(&pShellLink));

		if (SUCCEEDED(hres)) {
			pShellLink->SetPath(targetExe.string().c_str());
			pShellLink->SetWorkingDirectory(workingDirectory.string().c_str());
			pShellLink->SetDescription("Launch BOIII Vanilla");
			IPersistFile* pPersistFile = nullptr;
			hres = pShellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pPersistFile));

			if (SUCCEEDED(hres)) {
				wchar_t desktopPath[MAX_PATH];
				SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOP, FALSE);

				std::filesystem::path shortcutPath = std::filesystem::path(desktopPath) / (shortcutName + ".lnk");

				pPersistFile->Save(shortcutPath.c_str(), TRUE);
				pPersistFile->Release();
			}

			pShellLink->Release();
		}

		CoUninitialize();
	}

	void install_bo3folerauto(const std::filesystem::path& exePath)
	{
		try
		{
			std::filesystem::path steamPath = steam::SteamAPI_GetSteamInstallPath();
			std::filesystem::path bo3Folder = steamPath / "steamapps/common/Call of Duty Black Ops III";
			if (!std::filesystem::exists(bo3Folder))
			{
				throw std::runtime_error("Call of Duty Black Ops III folder not found in Steam directory. Consider copying it into that folder manually.");
			}
			std::filesystem::path destinationExe = bo3Folder / "boiii_vanilla.exe";
			std::filesystem::copy_file(exePath, destinationExe, std::filesystem::copy_options::overwrite_existing);
			install_desktopshortcut(destinationExe, "BOIII Vanilla", bo3Folder);
		}
		catch (const std::exception& e)
		{
			std::cerr << "Error: " << e.what() << std::endl;
		}
	}

	void install_copydir(const std::filesystem::path& source, const std::filesystem::path& destination)
	{
		try
		{
			std::filesystem::create_directories(destination);
			std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
		}
		catch (const std::exception& e)
		{
			throw std::runtime_error("Failed to copy the boiii_vanilla folder to Local AppData (try to do so yourself): " + std::string(e.what()));
		}
	}

	void update()
	{
		//instead of grabbing files online from the ezz client, it will grab files based on this version of the client instead locally due to the changes in LUI scripts. May modify this a bit later.
		const std::filesystem::path appdata_path = std::filesystem::path(getenv("LOCALAPPDATA")) / "boiii_vanilla";
		const std::filesystem::path exe_directory = std::filesystem::current_path() / "boiii_vanilla";
		const std::filesystem::path exe_path = std::filesystem::path("boiii.exe");
		if (!std::filesystem::exists(appdata_path))
		{
			if (std::filesystem::exists(exe_directory))
			{
				install_copydir(exe_directory, appdata_path);
				std::cout << "Successfully set up BOIII Vanilla Data." << std::endl;
			}
			else
			{
				throw std::runtime_error("boiii_vanilla folder needs to be in the same directory as this setup.");
			}
			if (std::filesystem::exists(exe_path))
			{
				install_bo3folerauto(exe_path);
				std::cout << "Successfully set up BOIII Vanilla." << std::endl;
			}
			else
			{
				throw std::runtime_error("boiii_vanilla.exe needs to be copied to the boiii_vanilla folder under the local appdata folder as well as the Black Ops III steam folder.");
			}
		}

		//original code
		if (utils::flags::has_flag("noupdate"))
		{
			return;
		}

		try
		{
			run(game::get_appdata_path());
		}
		catch (update_cancelled&)
		{
			TerminateProcess(GetCurrentProcess(), 0);
		}
		catch (...)
		{
		}
	}

	class component final : public generic_component
	{
	public:
		component()
		{
			this->update_thread_ = std::thread([this]
			{
				update();
			});
		}

		void pre_destroy() override
		{
			join();
		}

		void post_unpack() override
		{
			join();
		}

		component_priority priority() const override
		{
			return component_priority::updater;
		}

	private:
		std::thread update_thread_{};

		void join()
		{
			if (this->update_thread_.joinable())
			{
				this->update_thread_.join();
			}
		}
	};
}

REGISTER_COMPONENT(updater::component)
