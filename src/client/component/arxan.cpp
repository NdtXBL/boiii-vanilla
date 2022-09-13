#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "scheduler.hpp"

#include "steam/steam.hpp"
#include <utils/hook.hpp>

#include "utils/string.hpp"
#include "utils/hardware_breakpoint.hpp"

#define ProcessDebugPort 7
#define ProcessDebugObjectHandle 30 // WinXP source says 31?
#define ProcessDebugFlags 31 // WinXP source says 32?

namespace arxan
{
	namespace
	{
		constexpr auto pseudo_steam_id = 0x1337;
		const auto pseudo_steam_handle = reinterpret_cast<HANDLE>(reinterpret_cast<uint64_t>(INVALID_HANDLE_VALUE) -
			pseudo_steam_id);

		utils::hook::detour nt_close_hook;
		utils::hook::detour nt_query_system_information_hook;
		utils::hook::detour nt_query_information_process_hook;
		utils::hook::detour create_mutex_ex_a_hook;
		utils::hook::detour open_process_hook;
		utils::hook::detour create_thread_hook;
		utils::hook::detour get_thread_context_hook;
		utils::hook::detour zw_terminate_process_hook;
		utils::hook::detour get_proc_address_hook;

		void* original_first_tls_callback = nullptr;

		void** get_tls_callbacks()
		{
			const utils::nt::library game{};
			const auto& entry = game.get_optional_header()->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
			if (!entry.VirtualAddress || !entry.Size)
			{
				return nullptr;
			}

			const auto* tls_dir = reinterpret_cast<IMAGE_TLS_DIRECTORY*>(game.get_ptr() + entry.VirtualAddress);
			return reinterpret_cast<void**>(tls_dir->AddressOfCallBacks);
		}

		void disable_tls_callbacks()
		{
			auto* tls_callbacks = get_tls_callbacks();
			if (tls_callbacks)
			{
				original_first_tls_callback = *tls_callbacks;
			}

			utils::hook::set(tls_callbacks, nullptr);
		}

		void restore_tls_callbacks()
		{
			auto* tls_callbacks = get_tls_callbacks();
			if (tls_callbacks)
			{
				utils::hook::set(tls_callbacks, original_first_tls_callback);
			}
		}

		HANDLE WINAPI create_thread_stub(const LPSECURITY_ATTRIBUTES thread_attributes, const SIZE_T stack_size,
		                                 const LPTHREAD_START_ROUTINE start_address, const LPVOID parameter,
		                                 const DWORD creation_flags,
		                                 const LPDWORD thread_id)
		{
			if (utils::nt::library::get_by_address(start_address) == utils::nt::library{})
			{
				restore_tls_callbacks();

				create_thread_hook.clear();
				return CreateThread(thread_attributes, stack_size, start_address, parameter, creation_flags,
				                    thread_id);
			}

			return create_thread_hook.invoke<HANDLE>(thread_attributes, stack_size, start_address, parameter,
			                                         creation_flags, thread_id);
		}

		HANDLE process_id_to_handle(const DWORD pid)
		{
			return reinterpret_cast<HANDLE>(static_cast<DWORD64>(pid));
		}

		HANDLE WINAPI open_process_stub(const DWORD access, const BOOL inherit, const DWORD pid)
		{
			if (pid == pseudo_steam_id)
			{
				return pseudo_steam_handle;
			}

			return open_process_hook.invoke<HANDLE>(access, inherit, pid);
		}


		HANDLE create_mutex_ex_a_stub(const LPSECURITY_ATTRIBUTES attributes, const LPCSTR name, const DWORD flags,
		                              const DWORD access)
		{
			if (name == "$ IDA trusted_idbs"s || name == "$ IDA registry mutex $"s)
			{
				return nullptr;
			}

			return create_mutex_ex_a_hook.invoke<HANDLE>(attributes, name, flags, access);
		}

		bool remove_evil_keywords_from_string(const UNICODE_STRING& string)
		{
			static const std::wstring evil_keywords[] =
			{
				L"IDA",
				L"ida",
				L"HxD",
				L"cheatengine",
				L"Cheat Engine",
				L"x96dbg",
				L"x32dbg",
				L"x64dbg",
				L"Wireshark",
			};

			if (!string.Buffer || !string.Length)
			{
				return false;
			}

			std::wstring_view path(string.Buffer, string.Length / sizeof(string.Buffer[0]));

			bool modified = false;
			for (const auto& keyword : evil_keywords)
			{
				while (true)
				{
					const auto pos = path.find(keyword);
					if (pos == std::wstring::npos)
					{
						break;
					}

					modified = true;

					for (size_t i = 0; i < keyword.size(); ++i)
					{
						string.Buffer[pos + i] = L'a';
					}
				}
			}

			return modified;
		}

		bool remove_evil_keywords_from_string(wchar_t* str, const size_t length)
		{
			UNICODE_STRING unicode_string{};
			unicode_string.Buffer = str;
			unicode_string.Length = static_cast<uint16_t>(length);
			unicode_string.MaximumLength = unicode_string.Length;

			return remove_evil_keywords_from_string(unicode_string);
		}

		bool remove_evil_keywords_from_string(char* str, const size_t length)
		{
			std::string_view str_view(str, length);
			std::wstring wstr(str_view.begin(), str_view.end());

			if (!remove_evil_keywords_from_string(&wstr[0], wstr.size()))
			{
				return false;
			}

			const std::string regular_str(wstr.begin(), wstr.end());
			memcpy(str, regular_str.data(), length);

			return true;
		}


		int WINAPI get_window_text_a_stub(const HWND wnd, const LPSTR str, const int max_count)
		{
			std::wstring wstr{};
			wstr.resize(max_count);

			const auto res = GetWindowTextW(wnd, &wstr[0], max_count);
			if (res)
			{
				remove_evil_keywords_from_string(wstr.data(), res);

				const std::string regular_str(wstr.begin(), wstr.end());
				memset(str, 0, max_count);
				memcpy(str, regular_str.data(), res);
			}

			return res;
		}

		NTSTATUS NTAPI nt_query_system_information_stub(const SYSTEM_INFORMATION_CLASS system_information_class,
		                                                const PVOID system_information,
		                                                const ULONG system_information_length,
		                                                const PULONG return_length)
		{
			const auto status = nt_query_system_information_hook.invoke<NTSTATUS>(
				system_information_class, system_information, system_information_length, return_length);

			if (NT_SUCCESS(status))
			{
				if (system_information_class == SystemProcessInformation)
				{
					bool injected_steam = false;
					auto addr = static_cast<uint8_t*>(system_information);
					while (true)
					{
						const auto info = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(addr);
						remove_evil_keywords_from_string(info->ImageName);

						static const auto our_pid = process_id_to_handle(GetCurrentProcessId());

						if (!injected_steam && info->UniqueProcessId != our_pid)
						{
							static wchar_t steam_path[] = L"steam.exe";

							info->UniqueProcessId = process_id_to_handle(pseudo_steam_id);
							info->ImageName.Buffer = steam_path;
							info->ImageName.Length = static_cast<uint16_t>(wcslen(steam_path) * 2u);
							info->ImageName.MaximumLength = info->ImageName.Length;

							injected_steam = true;
						}

						if (!info->NextEntryOffset)
						{
							break;
						}

						addr = addr + info->NextEntryOffset;
					}
				}
			}

			return status;
		}

		bool handle_pseudo_steam_process(const HANDLE handle, const PROCESSINFOCLASS info_class,
		                                 const PVOID info,
		                                 const ULONG info_length, const PULONG ret_length, NTSTATUS* status)
		{
			if (handle != pseudo_steam_handle || static_cast<int>(info_class) != 43)
			{
				return false;
			}

			const auto steam_folder = steam::SteamAPI_GetSteamInstallPath();
			const auto steam_path = steam_folder + "\\steam.exe"s;
			const std::wstring wide_path(steam_path.begin(), steam_path.end());

			const auto required_size = static_cast<ULONG>((wide_path.size() + 1u) * 2u + sizeof(UNICODE_STRING));

			if (ret_length)
			{
				*ret_length = required_size;
			}

			if (info_length < required_size)
			{
				*status = static_cast<LONG>(0xC0000004);
				return true;
			}

			memset(info, 0, info_length);

			auto& str = *static_cast<UNICODE_STRING*>(info);
			str.Buffer = reinterpret_cast<wchar_t*>(&str + 1);
			str.Length = static_cast<uint16_t>(wide_path.size() * 2u);
			str.MaximumLength = str.Length;

			memcpy(str.Buffer, wide_path.data(), str.Length);

			*status = 0;
			return true;
		}

		NTSTATUS WINAPI nt_query_information_process_stub(const HANDLE handle, const PROCESSINFOCLASS info_class,
		                                                  const PVOID info,
		                                                  const ULONG info_length, const PULONG ret_length)
		{
			NTSTATUS status{0};
			if (handle_pseudo_steam_process(handle, info_class, info, info_length, ret_length, &status))
			{
				return status;
			}

			status = nt_query_information_process_hook.invoke<NTSTATUS>(handle, info_class, info, info_length,
			                                                            ret_length);

			if (NT_SUCCESS(status))
			{
				if (info_class == ProcessBasicInformation)
				{
					static_cast<PPROCESS_BASIC_INFORMATION>(info)->Reserved3 = process_id_to_handle(pseudo_steam_id);
				}
				else if (info_class == ProcessDebugObjectHandle)
				{
					*static_cast<HANDLE*>(info) = nullptr;
					return static_cast<LONG>(0xC0000353);
				}
				else if (info_class == ProcessImageFileName || static_cast<int>(info_class) == 43 /* ? */)
				{
					remove_evil_keywords_from_string(*static_cast<UNICODE_STRING*>(info));
				}
				else if (info_class == ProcessDebugPort)
				{
					*static_cast<HANDLE*>(info) = nullptr;
				}
				else if (info_class == ProcessDebugFlags)
				{
					*static_cast<ULONG*>(info) = 1;
				}
			}

			return status;
		}

		NTSTATUS NTAPI nt_close_stub(const HANDLE handle)
		{
			if (handle == pseudo_steam_handle)
			{
				return 0;
			}

			char info[16];
			if (NtQueryObject(handle, OBJECT_INFORMATION_CLASS(4), &info, 2, nullptr) >= 0 && size_t(handle) != 0x12345)
			{
				return nt_close_hook.invoke<NTSTATUS>(handle);
			}

			return STATUS_INVALID_HANDLE;
		}

		void hide_being_debugged()
		{
			auto* const peb = reinterpret_cast<PPEB>(__readgsqword(0x60));
			peb->BeingDebugged = false;
			*reinterpret_cast<PDWORD>(LPSTR(peb) + 0xBC) &= ~0x70;
		}

		void restore_debug_functions()
		{
			static const char* functions[] = {
				"DbgBreakPoint",
				"DbgUserBreakPoint",
				"DbgUiConnectToDbg",
				"DbgUiContinue",
				"DbgUiConvertStateChangeStructure",
				"DbgUiDebugActiveProcess",
				"DbgUiGetThreadDebugObject",
				"DbgUiIssueRemoteBreakin",
				"DbgUiRemoteBreakin",
				"DbgUiSetThreadDebugObject",
				"DbgUiStopDebugging",
				"DbgUiWaitStateChange",
				"DbgPrintReturnControlC",
				"DbgPrompt",
			};

			using buffer = uint8_t[15];
			static buffer buffers[ARRAYSIZE(functions)] = {};
			static bool loaded = false;

			const utils::nt::library ntdll("ntdll.dll");

			for (auto i = 0u; i < ARRAYSIZE(functions); ++i)
			{
				const auto func = ntdll.get_proc<void*>(functions[i]);
				if (!loaded)
				{
					memcpy(buffers[i], func, sizeof(buffer));
				}
				else
				{
					utils::hook::copy(func, buffers[i], sizeof(buffer));
				}
			}

			loaded = true;
		}

		const std::pair<uint8_t*, size_t>& get_text_section()
		{
			static const std::pair<uint8_t*, size_t> text = []() -> std::pair<uint8_t*, size_t>
				{
					const utils::nt::library game{};
					for (const auto& section : game.get_section_headers())
					{
						std::string name(reinterpret_cast<char*>(section->Name), sizeof(section->Name));
						while (!name.empty() && !name.back()) name.pop_back();

						if (name == ".text"s)
						{
							return {game.get_ptr() + section->VirtualAddress, section->Misc.VirtualSize};
						}
					}

					return {nullptr, 0};
				}
				();

			return text;
		}

		bool was_in_text(const ULONG_PTR addr)
		{
			return addr >= reinterpret_cast<ULONG_PTR>(get_text_section().first) && addr <= reinterpret_cast<ULONG_PTR>(
				get_text_section().first +
				get_text_section().second);
		}

		void protect_text()
		{
			DWORD old_protect{};
			VirtualProtect(get_text_section().first, get_text_section().second, PAGE_EXECUTE_READ, &old_protect);
		}

		void unprotect_text()
		{
			DWORD old_protect{};
			VirtualProtect(get_text_section().first, get_text_section().second, PAGE_EXECUTE_READWRITE, &old_protect);
		}

		LONG WINAPI exception_filter(const LPEXCEPTION_POINTERS info)
		{
			/*static thread_local struct
			{
				bool needs_protect_change = false;
				bool had_single_step = false;
 			} analysis_context;*/

			if (info->ExceptionRecord->ExceptionCode == STATUS_INVALID_HANDLE)
			{
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			/*if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP)
			{
				//utils::thread::suspend_other_threads();
				//restore_debug_functions();
				//MessageBoxA(0, "SS", 0, 0);
				OutputDebugStringA("SINGLESTEP!\n");
				return EXCEPTION_CONTINUE_EXECUTION;
			}*/

			/*if (info->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION)
			{
				utils::thread::suspend_other_threads();
				restore_debug_functions();
				MessageBoxA(nullptr, utils::string::va("AV at: %llX %llX",
				                                       info->ContextRecord->Rip, reverse_g(info->ContextRecord->Rip)),
				            nullptr, 0);
				return EXCEPTION_CONTINUE_EXECUTION;
			}*/

			/*if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP)
			{
				if (!analysis_context.needs_protect_change)
				{
					return EXCEPTION_CONTINUE_SEARCH;
				}

				analysis_context.needs_protect_change = false;

				if (!analysis_context.had_single_step)
				{
					info->ContextRecord->EFlags &= 0x0100;
				}

				protect_text();
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			if (info->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION)
			{
				// Was write?
				if (info->ExceptionRecord->ExceptionInformation[0] != 1)
				{
					return EXCEPTION_CONTINUE_SEARCH;
				}

				const auto addr = info->ExceptionRecord->ExceptionInformation[1];
				if (!was_in_text(addr))
				{
					return EXCEPTION_CONTINUE_SEARCH;
				}

				analysis_context.needs_protect_change = true;
				analysis_context.had_single_step = info->ContextRecord->EFlags & 0x0100;
				info->ContextRecord->EFlags |= 0x0100;

				OutputDebugStringA(utils::string::va("Switch at: %llX -> %llX (%llX -> %llX)", addr, reverse_g(addr),
				                                     info->ContextRecord->Rip, reverse_g(info->ContextRecord->Rip)));

				unprotect_text();
				return EXCEPTION_CONTINUE_EXECUTION;
				//restore_debug_functions();
			}*/

			return EXCEPTION_CONTINUE_SEARCH;
		}

		const char* get_command_line_a_stub()
		{
			static auto cmd = []
			{
				std::string cmd_line = GetCommandLineA();
				if (!strstr(cmd_line.data(), "fs_game"))
				{
					cmd_line += " +set fs_game \"boiii\"";
				}

				return cmd_line;
			}();

			return cmd.data();
		}
	}

	int WINAPI get_system_metrics_stub(const int index)
	{
		if (SM_REMOTESESSION == index)
		{
			return 0;
		}

		return GetSystemMetrics(index);
	}

	BOOL WINAPI get_thread_context_stub(const HANDLE thread_handle, const LPCONTEXT context)
	{
		constexpr auto debug_registers_flag = (CONTEXT_DEBUG_REGISTERS & ~CONTEXT_AMD64);
		if (context->ContextFlags & debug_registers_flag)
		{
			auto* source = _ReturnAddress();
			const auto game = utils::nt::library{};
			const auto source_module = utils::nt::library::get_by_address(source);

			if (source_module == game)
			{
				context->ContextFlags &= ~debug_registers_flag;
			}
		}

		return get_thread_context_hook.invoke<BOOL>(thread_handle, context);
	}

	uint64_t get_integrity_data_qword(const uint8_t* address)
	{
		OutputDebugStringA(utils::string::va("8 bytes -> %p", address));
		const auto og_data = utils::hook::query_original_data(address, 8);
		return *reinterpret_cast<const uint64_t*>(og_data.data());
	}

	uint32_t get_integrity_data_dword(const uint8_t* address)
	{
		OutputDebugStringA(utils::string::va("4 bytes -> %p", address));
		const auto og_data = utils::hook::query_original_data(address, 4);
		return *reinterpret_cast<const uint32_t*>(og_data.data());
	}

	uint8_t get_integrity_data_byte(const uint8_t* address)
	{
		OutputDebugStringA(utils::string::va("1 bytes -> %p", address));
		const auto og_data = utils::hook::query_original_data(address, 1);
		return og_data[0];
	}

	void patch_check_type_1_direct()
	{
		auto patch_addr = [](uint8_t* addr)
		{
			// Skip false positives
			// Prefixed 0x41 encodes a different instruction
			if (addr[-1] == 0x41)
			{
				return;
			}

			utils::hook::jump(addr, utils::hook::assemble([addr](utils::hook::assembler& a)
			{
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_dword);

				a.mov(rcx, qword_ptr(rsp, 128));
				a.mov(ecx, eax);
				a.mov(qword_ptr(rsp, 128), rcx);

				a.popad64();
				a.pop(rax);

				a.embedUInt8(addr[3]);
				a.embedUInt8(addr[4]);
				a.embedUInt8(addr[5]);

				a.jmp(addr + 5);
			}));
		};

		// mov [rbp+??h], eax
		auto checks = "8B 00 89 45 ??"_sig;
		for (auto* addr : checks)
		{
			patch_addr(addr);
		}

		// xor eax, [rbp+??h]
		checks = "8B 00 33 45 ??"_sig;
		for (auto* addr : checks)
		{
			patch_addr(addr);
		}
	}

	void patch_check_type_1_indirect()
	{
		auto patch_addr = [](uint8_t* addr)
		{
			const auto rex_prefixed = *addr == 0x48;
			const auto jump_target = utils::hook::follow_branch(addr + (rex_prefixed ? 3 : 2));

			utils::hook::jump(addr, utils::hook::assemble([addr, jump_target, rex_prefixed](utils::hook::assembler& a)
			{
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);

				if (rex_prefixed)
				{
					a.call_aligned(get_integrity_data_dword);

					a.mov(rcx, qword_ptr(rsp, 128));
					a.mov(ecx, eax);
					a.mov(qword_ptr(rsp, 128), rcx);
				}
				else
				{
					a.mov(qword_ptr(rsp, 128), rax);
				}

				a.popad64();
				a.pop(rax);

				a.jmp(jump_target);
			}));
		};

		// mov rax, [rax]; jmp ...
		auto checks = "48 8B 00 E9"_sig;
		for (auto* addr : checks)
		{
			patch_addr(addr);
		}

		// mov eax, [rax]; jmp ...
		checks = "8B 00 E9"_sig;
		for (auto* addr : checks)
		{
			patch_addr(addr);
		}
	}

	void patch_check_type_2_direct()
	{
		const auto checks = "0F B6 00 0F B6 C0"_sig;
		for (auto* addr : checks)
		{
			utils::hook::jump(addr, utils::hook::assemble([addr](utils::hook::assembler& a)
			{
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_byte);

				a.mov(rcx, qword_ptr(rsp, 128));
				a.movzx(ecx, al);
				a.mov(qword_ptr(rsp, 128), rcx);

				a.popad64();
				a.pop(rax);

				a.movzx(eax, al);
				a.jmp(addr + 6);
			}));
		}
	}

	void patch_check_type_2_indirect()
	{
		const auto checks = "0F B6 00 E9"_sig;
		for (auto* addr : checks)
		{
			const auto jump_target = utils::hook::follow_branch(addr + 3);

			utils::hook::jump(addr, utils::hook::assemble([jump_target](utils::hook::assembler& a)
			{
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_byte);

				a.mov(rcx, qword_ptr(rsp, 128));
				a.movzx(ecx, al);
				a.mov(qword_ptr(rsp, 128), rcx);

				a.popad64();
				a.pop(rax);

				a.jmp(jump_target);
			}));
		}
	}

	void patch_check_type_4_direct()
	{
		const auto checks = "48 8B 04 10 48 89 45 20"_sig;
		for (auto* addr : checks)
		{
			utils::hook::jump(addr, utils::hook::assemble([addr](utils::hook::assembler& a)
			{
				a.mov(rax, qword_ptr(rax, rdx));
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_qword);
				a.mov(qword_ptr(rsp, 128), rax);

				a.popad64();
				a.pop(rax);

				a.mov(qword_ptr(rbp, 0x20), rax);
				a.jmp(addr + 8);
			}));
		}
	}

	void patch_check_type_4_indirect()
	{
		const auto checks = "48 8B 04 10 E9"_sig;
		for (auto* addr : checks)
		{
			const auto jump_target = utils::hook::follow_branch(addr + 4);

			utils::hook::jump(addr, utils::hook::assemble([jump_target](utils::hook::assembler& a)
			{
				a.mov(rax, qword_ptr(rax, rdx));
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_qword);
				a.mov(qword_ptr(rsp, 128), rax);

				a.popad64();
				a.pop(rax);

				a.jmp(jump_target);
			}));
		}
	}

	void patch_check_type_5_direct()
	{
		const auto checks = "0F B6 00 88 02"_sig;
		for (auto* addr : checks)
		{
			// Skip false positives
			// Prefixed 0x41 encodes a different instruction
			if (addr[-1] == 0x41)
			{
				continue;
			}

			utils::hook::jump(addr, utils::hook::assemble([addr](utils::hook::assembler& a)
			{
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_byte);

				a.mov(rcx, qword_ptr(rsp, 128));
				a.movzx(ecx, al);
				a.mov(qword_ptr(rsp, 128), rcx);

				a.popad64();
				a.pop(rax);

				a.mov(byte_ptr(rdx), al);
				a.jmp(addr + 5);
			}));
		}
	}


	void patch_check_type_5_indirect()
	{
		const auto checks = "0F B6 00 E9"_sig;
		for (auto* addr : checks)
		{
			// Skip false positives
			// Prefixed 0x41 encodes a different instruction
			if (addr[-1] == 0x41)
			{
				continue;
			}

			const auto jump_target = utils::hook::follow_branch(addr + 4);

			utils::hook::jump(addr, utils::hook::assemble([jump_target](utils::hook::assembler& a)
			{
				a.push(rax);
				a.pushad64();

				a.mov(rcx, rax);
				a.call_aligned(get_integrity_data_byte);

				a.mov(rcx, qword_ptr(rsp, 128));
				a.movzx(ecx, al);
				a.mov(qword_ptr(rsp, 128), rcx);

				a.popad64();
				a.pop(rax);

				a.jmp(jump_target);
			}));
		}
	}

	NTSTATUS NTAPI get_proc_address_stub(const HMODULE module_handle, const PANSI_STRING function_name,
	                                     const WORD oridinal,
	                                     PVOID* function_address, const BOOL b_value,
	                                     PVOID* callback_address)
	{
		OutputDebugStringA(utils::string::va("Proc: %s %X\n",
		                                     (function_name && function_name->Buffer)
			                                     ? function_name->Buffer
			                                     : "(null)", static_cast<DWORD>(oridinal)));

		return get_proc_address_hook.invoke<NTSTATUS>(module_handle, function_name, oridinal, function_address, b_value,
		                                              callback_address);
	}

	NTSTATUS zw_terminate_process_stub(const HANDLE process_handle, const NTSTATUS exit_status)
	{
		MessageBoxA(nullptr, "TERMINATING", nullptr, 0);
		return zw_terminate_process_hook.invoke<NTSTATUS>(process_handle, exit_status);
	}

	class component final : public component_interface
	{
	public:
		component()
		{
#ifndef NDEBUG
			auto* dll_characteristics = &utils::nt::library().get_optional_header()->DllCharacteristics;
			utils::hook::set<WORD>(dll_characteristics, *dll_characteristics | IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
#endif
		}

		void pre_start() override
		{
			disable_tls_callbacks();
			restore_debug_functions();

			hide_being_debugged();
			scheduler::loop(hide_being_debugged, scheduler::pipeline::async);

			create_thread_hook.create(CreateThread, create_thread_stub);
			create_mutex_ex_a_hook.create(CreateMutexExA, create_mutex_ex_a_stub);

			const utils::nt::library ntdll("ntdll.dll");
			nt_close_hook.create(ntdll.get_proc<void*>("NtClose"), nt_close_stub);

			const auto nt_query_information_process = ntdll.get_proc<void*>("NtQueryInformationProcess");
			nt_query_information_process_hook.create(nt_query_information_process,
			                                         nt_query_information_process_stub);

			const auto nt_query_system_information = ntdll.get_proc<void*>("NtQuerySystemInformation");
			nt_query_system_information_hook.create(nt_query_system_information, nt_query_system_information_stub);
			nt_query_system_information_hook.move();

			open_process_hook.create(OpenProcess, open_process_stub);

#ifndef NDEBUG
			auto* get_thread_context_func = utils::nt::library("kernelbase.dll").get_proc<void*>("GetThreadContext");
			get_thread_context_hook.create(get_thread_context_func, get_thread_context_stub);
#endif

			utils::hook::copy(this->window_text_buffer_, GetWindowTextA, sizeof(this->window_text_buffer_));
			utils::hook::jump(GetWindowTextA, get_window_text_a_stub, true, true);
			utils::hook::move_hook(GetWindowTextA);

			AddVectoredExceptionHandler(1, exception_filter);

			auto* sys_met_import = utils::nt::library{}.get_iat_entry("user32.dll", "GetSystemMetrics");
			if (sys_met_import) utils::hook::set(sys_met_import, get_system_metrics_stub);

			// TODO: Remove as soon as real hooking works
			auto* get_cmd_import = utils::nt::library{}.get_iat_entry("kernel32.dll", "GetCommandLineA");
			if (get_cmd_import) utils::hook::set(get_cmd_import, get_command_line_a_stub);
			//zw_terminate_process_hook.create(ntdll.get_proc<void*>("ZwTerminateProcess"), zw_terminate_process_stub);
			//zw_terminate_process_hook.move();

			//auto* gpafc = ntdll.get_proc<void*>("LdrGetProcedureAddressForCaller");
			//get_proc_address_hook.create(gpafc, get_proc_address_stub);
			//get_proc_address_hook.move();
		}

		void post_unpack() override
		{
			//restore_debug_functions();
			/*
			patch_check_type_1_direct();
			patch_check_type_1_indirect();
			patch_check_type_2_direct();
			patch_check_type_2_indirect();
			patch_check_type_4_direct();
			patch_check_type_4_indirect();
			patch_check_type_5_direct();
			patch_check_type_5_indirect();
			MessageBoxA(0, "done", 0, 0);
			*/

			//protect_text();

			/*auto tid = GetCurrentThreadId();
			std::thread([tid]()
			{
				MessageBoxA(0, 0, 0, 0);
				//utils::hook::set<uint8_t>(0x1423339C0_g, 0xC3);
				utils::hardware_breakpoint::activate(0x1423339C0_g, 4,
				                                     utils::hardware_breakpoint::read_write, tid);
			}).detach();*/

			// Some integrity check patches. More to come.

			/*
			utils::hook::nop(0x142AA20A1_g, 4);
			utils::hook::set<uint32_t>(0x15BDEC91F_g, 0xda894890);

			utils::hook::nop(0x15E4EBFA6_g, 4);
			utils::hook::set<uint32_t>(0x15EA17E28_g, 0xd1894890);

			utils::hook::nop(0x15B7F5209_g, 6);
			utils::hook::set<uint32_t>(0x15EFBB508_g, 0xc3894890);

			utils::hook::set<uint32_t>(0x15D0379CC_g, 0xda894890); // rdx, rbx
			utils::hook::set<uint32_t>(0x15D1177B8_g, 0xd1894890); // rcx, rdx
			utils::hook::set<uint32_t>(0x15BFFF30D_g, 0xda894890); // rdx, rbx
			utils::hook::set<uint32_t>(0x15DE3AAE7_g, 0xc8894890); // rax, rcx
			utils::hook::set<uint32_t>(0x15E48F80C_g, 0xc3894890); // rbx, rax
			*/
		}

		void pre_destroy() override
		{
			utils::hook::copy(GetWindowTextA, this->window_text_buffer_, sizeof(this->window_text_buffer_));
			nt_query_system_information_hook.clear();
			nt_query_information_process_hook.clear();
			nt_close_hook.clear();
			create_mutex_ex_a_hook.clear();
			create_thread_hook.clear();
			open_process_hook.clear();
			get_thread_context_hook.clear();
			zw_terminate_process_hook.clear();
			get_proc_address_hook.clear();
		}

		int priority() override
		{
			return 9999;
		}

	private:
		uint8_t window_text_buffer_[15]{};
	};
}

REGISTER_COMPONENT(arxan::component)
