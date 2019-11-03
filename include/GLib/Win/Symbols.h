#pragma once

#include "GLib/Win/Local.h"
#include "GLib/Win/Process.h"
#include "GLib/compat.h"
#include "GLib/scope.h"

#define _NO_CVCONST_H
#include <DbgHelp.h>
#pragma comment(lib , "DbgHelp.lib")

#include <array>
#include <filesystem>
#include <functional>
#include <sstream>

namespace GLib::Win::Symbols
{
	namespace Detail
	{
		struct Cleanup
		{
			void operator()(HANDLE handle) const noexcept
			{
				Util::WarnAssertTrue(::SymCleanup(handle), "SymCleanup failed");
			}
		};
		using SymbolHandle = std::unique_ptr<void, Cleanup>;

		inline Handle Duplicate(HANDLE handle)
		{
			HANDLE duplicatedHandle;
			Util::AssertTrue(::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(),
				&duplicatedHandle, 0, FALSE, DUPLICATE_SAME_ACCESS), "DuplicateHandle");
			return Handle { duplicatedHandle };
		}

		inline ULONG64 ConvertBase(void * baseValue)
		{
			return reinterpret_cast<ULONG64>(baseValue); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
		}
	}

	class Symbol
	{
		ULONG index {};
		ULONG typeIndex {};
		enum SymTagEnum tag {};
		std::string name;
		DWORD64 displacement {};

	public:
		Symbol() = default;

		Symbol(ULONG index, ULONG typeIndex, enum SymTagEnum tag, std::string name, DWORD64 displacement)
			: index(index)
			, typeIndex(typeIndex)
			, tag(tag)
			, name(move(name))
			, displacement(displacement)
		{}

		ULONG Index() const
		{
			return index;
		}

		void Index(ULONG value)
		{
			index = value;
		}

		ULONG TypeIndex() const
		{
			return typeIndex;
		}

		void TypeIndex(ULONG value)
		{
			typeIndex = value;
		}

		enum SymTagEnum Tag() const
		{
			return tag;
		}

		void Tag(enum SymTagEnum value)
		{
			tag = value;
		}

		const std::string & Name()
		{
			return name;
		}

		void Name(std::string value)
		{
			name = move(value);
		}

		DWORD64 Displacement() const
		{
			return displacement;
		}
	};

	struct Line
	{
		unsigned int lineNumber;
		std::string fileName;
		uint64_t Address;
		unsigned int displacement;
	};

	class SymProcess
	{
		Process process;
		Detail::SymbolHandle symbolHandle;
		uint64_t baseOfImage{};

	public:
		SymProcess(Handle && handle, uint64_t baseOfImage)
			: process(std::move(handle))
			, symbolHandle(process.Handle().get())
			, baseOfImage(baseOfImage)
		{}

		static SymProcess CurrentProcess()
		{
			return GetProcess(::GetCurrentProcess(), 0, true);
		}

		static SymProcess GetProcess(HANDLE handle, uint64_t baseOfImage, bool invasive)
		{
			auto duplicate = Detail::Duplicate(handle);

			std::ostringstream searchPath;

			auto const processPath = std::filesystem::path(FileSystem::PathOfProcessHandle(duplicate.get())).parent_path().u8string();
			searchPath << processPath << ";";

			// set sym opts, add exe to sym path, legacy code, still needed?
			if (auto value = Compat::GetEnv("_NT_SYMBOL_PATH"))
			{
				searchPath << *value << ";";
			}

			if (auto value = Compat::GetEnv("PATH"))
			{
				searchPath << *value << ";";
			}

			// when debugging processHandle and enumerateModules = true, get errorCode=0x8000000d : An illegal state change was requested
			Util::AssertTrue(::SymInitializeW(duplicate.get(), Cvt::a2w(searchPath.str()).c_str(), invasive ? TRUE : FALSE), "SymInitialize failed");
			return { std::move(duplicate), baseOfImage};
		}

		const Handle & Handle() const
		{
			return process.Handle();
		}

		DWORD Id() const
		{
			return process.Id();
		}

		void ReadMemory(uint64_t address, void * buffer, size_t size, bool fromBase = true) const
		{
			process.ReadMemory(fromBase ? baseOfImage + address : address, buffer, size);
		}

		void WriteMemory(uint64_t address, const void * buffer, size_t size, bool fromBase = true) const
		{
			process.WriteMemory(fromBase ? baseOfImage + address : address, buffer, size);
		}

		template <typename T> T Read(uint64_t address, bool absolute = false) const
		{
			T value;
			ReadMemory(address, &value, sizeof(T), absolute);
			return value;
		}

		template <typename T> void Write(uint64_t address, const T & value, bool absolute = false) const
		{
			WriteMemory(address, &value, sizeof(T), absolute);
		}

		std::optional<Symbol> TryGetSymbolFromAddress(uint64_t address) const
		{
			std::optional<Symbol> symbol;

			std::array<SYMBOL_INFOW, 2 + MAX_SYM_NAME * sizeof(wchar_t) / sizeof(SYMBOL_INFO)> buffer {};
			auto const symbuf = buffer.data();
			symbuf->SizeOfStruct = sizeof(SYMBOL_INFOW);
			symbuf->MaxNameLen = MAX_SYM_NAME;
			DWORD64 displacement;
			BOOL result = ::SymFromAddrW(process.Handle().get(), address, &displacement, symbuf);
			if (Util::WarnAssertTrue(result, "SymFromAddrW"))
			{
				symbol = { symbuf->Index, symbuf->TypeIndex, static_cast<enum SymTagEnum>(symbuf->Tag),
						Cvt::w2a(std::wstring_view{static_cast<const wchar_t *>(symbuf->Name)}), displacement };
			}
			return move(symbol);
		}

		std::optional<Line> TryGetLineFromAddress(uint64_t address) const
		{
			std::optional<Line> line;

			IMAGEHLP_LINEW64 tmpLine{sizeof(IMAGEHLP_LINEW64)};
			DWORD displacement;
			BOOL result = ::SymGetLineFromAddrW64(process.Handle().get(), address, &displacement, &tmpLine);
			if (Util::WarnAssertTrue(result, "SymFromAddrW"))
			{
				line = { tmpLine.LineNumber, Cvt::w2a(tmpLine.FileName), tmpLine.Address, displacement };
			}
			return move(line);
		}

		bool TryGetClassParent(const Symbol & symbol, Symbol & result) const
		{
			DWORD typeIndexOfClassParent;

			// docs say TypeId param should be the TypeIndex member of returned SYMBOL_INFO
			// and the result from TI_GET_CLASSPARENTID is "The type index of the class parent."
			// so we then get TI_GET_SYMINDEX for indexOfClassParent
			// but seems to get indexOfClassParent having the same value of typeIndexOfClassParent
			if (::SymGetTypeInfo(process.Handle().get(), baseOfImage, symbol.TypeIndex(), TI_GET_CLASSPARENTID, &typeIndexOfClassParent) == FALSE)
			{
				return false;
			}

			DWORD indexOfClassParent;
			if (::SymGetTypeInfo(process.Handle().get(), baseOfImage, typeIndexOfClassParent, TI_GET_SYMINDEX, &indexOfClassParent) == FALSE)
			{
				return false;
			}

			Local<WCHAR> name;
			Util::AssertTrue(::SymGetTypeInfo(process.Handle().get(), baseOfImage, indexOfClassParent, TI_GET_SYMNAME, name.GetPtr().Address()), "SymGetTypeInfo");
			result.Index(indexOfClassParent);
			result.TypeIndex(typeIndexOfClassParent);
			result.Name(Cvt::w2a(std::wstring_view{name.Get()}));
			return true;
		}

	private:
		uint64_t Address(uint64_t address, bool fromBase) const
		{
			return fromBase ? baseOfImage + address : address;
		}
	};

	class Engine
	{
		std::unordered_map<DWORD, SymProcess> handles;

	public:
		Engine()
		{
			auto flags = SYMOPT_DEBUG | static_cast<DWORD>(SYMOPT_UNDNAME) | static_cast<DWORD>(SYMOPT_LOAD_LINES);
			::SymSetOptions(::SymGetOptions() | flags);
		}

		SymProcess & AddProcess(DWORD processId, HANDLE processHandle, uint64_t baseOfImage, HANDLE imageFile, const std::string & imageName)
		{
			SymProcess sp = SymProcess::GetProcess(processHandle, baseOfImage, false);

			DWORD64 const loadBase = ::SymLoadModuleExW(sp.Handle().get(), imageFile, Cvt::a2w(imageName).c_str(), nullptr,
				static_cast<DWORD64>(baseOfImage), 0, nullptr, 0);
				Util::AssertTrue(0 != loadBase, "SymLoadModuleEx failed");

			return handles.emplace(processId, std::move(sp)).first->second;
		}

		const SymProcess & GetProcess(DWORD processId) const
		{
			auto const it = handles.find(processId);
			if (it == handles.end())
			{
				throw std::runtime_error("Process not found");
			}
			return it->second;
		}

		void RemoveProcess(DWORD processId)
		{
			if (handles.erase(processId) == 0)
			{
				throw std::runtime_error("Process not found");
			}
		}

		void SourceFiles(std::function<void(PSOURCEFILEW)> f, HANDLE process, void * base) const
		{
			(void) this;
			Util::AssertTrue(::SymEnumSourceFilesW(process, Detail::ConvertBase(base), nullptr, EnumSourceFiles, &f),
				"EnumSourceFiles failed");
		}

		void Lines(std::function<void(PSRCCODEINFOW)> f, HANDLE process, void * base) const
		{
			(void) this;
			auto result = ::SymEnumLinesW(process, Detail::ConvertBase(base), nullptr, nullptr, EnumLines, &f);
			Util::AssertTrue(result == TRUE || ::GetLastError() == ERROR_NOT_SUPPORTED, "SymEnumLines failed");
		}

		template <typename Inserter>
		void Processes(Inserter && inserter) const
		{
			(void) this;
			std::function<void(HANDLE)> f = [&](HANDLE h) { *inserter++ = h; };
			Util::AssertTrue(::SymEnumProcesses(EnumProcesses, &f), "SymEnumProcesses failed");
		}

	private:
		static BOOL CALLBACK EnumSourceFiles(PSOURCEFILEW sourceFileInfo, PVOID context)
		{
			(*static_cast<std::function<void(PSOURCEFILEW)>*>(context))(sourceFileInfo);
			return TRUE;
		}

		static BOOL CALLBACK EnumLines(PSRCCODEINFOW lineInfo, PVOID context)
		{
			(*static_cast<std::function<void(PSRCCODEINFOW)>*>(context))(lineInfo);
			return TRUE;
		}

		static BOOL CALLBACK EnumProcesses(HANDLE hProcess, PVOID context)
		{
			(*static_cast<std::function<void(HANDLE)>*>(context))(hProcess);
			return TRUE;
		}
	};
}