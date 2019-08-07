#include "pch.h"

#include "Coverage.h"

#include "FileCoverageData.h"
#include "Function.h"
#include "HtmlReport.h"

#include <GLib/XmlPrinter.h>

#include <fstream>

WideStrings Coverage::a2w(const Strings& strings)
{
	WideStrings wideStrings;
	std::transform(strings.begin(), strings.end(), std::inserter(wideStrings, wideStrings.begin()), GLib::Cvt::a2w);
	return wideStrings;
}

void Coverage::AddLine(const std::wstring & fileName, unsigned lineNumber, const GLib::Win::Symbols::SymProcess & process, DWORD64 address)
{
	// filter out unknown source lines, looks like these cause the out of memory exceptions in ReportGenerator
	if (lineNumber == FooFoo || lineNumber == FeeFee)
	{
		return;
	}

	if (wideFiles.find(fileName) == wideFiles.end())
	{
		bool include = includes.empty();
		bool exclude = false;

		for (const auto & value : includes)
		{
			bool const isMatch = ::_wcsnicmp(value.c_str(), fileName.c_str(), value.size()) == 0;
			include |= isMatch;
		}

		for (const auto & value : excludes)
		{
			bool const isMatch = ::_wcsnicmp(value.c_str(), fileName.c_str(), value.size()) == 0;
			exclude |= isMatch;
		}

		if (!include || exclude)
		{
			return;
		}

		// functional change to symbol api or compiler change? file names as of 28jan 2019 are all lower case
		wideFiles.insert(fileName);
	}

	auto it = addresses.find(address);
	if (it == addresses.end())
	{
		const auto oldByte = process.Read<unsigned char>(address);
		it = addresses.emplace(address, oldByte).first;
	}
	it->second.AddFileLine(fileName, lineNumber);

	process.Write(address, debugBreakByte);
}

void Coverage::OnCreateProcess(DWORD processId, DWORD threadId, const CREATE_PROCESS_DEBUG_INFO& info)
{
	Debugger::OnCreateProcess(processId, threadId, info);

	const GLib::Win::Symbols::SymProcess & process = Symbols().GetProcess(processId);

	// todo, handle child processes, currently disabled via DEBUG_ONLY_THIS_PROCESS

	// timing check, 6s with a2w, 1s with no convert, unordered map 1.6s (needs tolower on string for hash)
	//using Clock = std::chrono::high_resolution_clock;
	//auto startValue = Clock::now();

	Symbols().Lines([&](PSRCCODEINFOW lineInfo)
	{
		AddLine(static_cast<const wchar_t *>(lineInfo->FileName), lineInfo->LineNumber, process, lineInfo->Address);
	}, process.Handle().get(), info.lpBaseOfImage);

	// use flog...
	// auto now = Clock::now();
	// std::chrono::duration<double> elapsedSeconds = now - startValue;
	// auto elapsed = elapsedSeconds.count();
	// std::cout << "Symbol lines processed in " << elapsed << " s" << std::endl;

	CREATE_THREAD_DEBUG_INFO threadInfo {};
	threadInfo.hThread = info.hThread;
	OnCreateThread(processId, threadId, threadInfo);
}

void Coverage::OnExitProcess(DWORD processId, DWORD threadId, const EXIT_PROCESS_DEBUG_INFO& info)
{
	CreateReport(processId); // todo just cache data to memory, and do report at exit
	
	Debugger::OnExitProcess(processId, threadId, info);
}

void Coverage::OnCreateThread(DWORD processId, DWORD threadId, const CREATE_THREAD_DEBUG_INFO& info)
{
	UNREFERENCED_PARAMETER(processId);

	threads.insert({ threadId, info.hThread });
}

void Coverage::OnExitThread(DWORD processId, DWORD threadId, const EXIT_THREAD_DEBUG_INFO& info)
{
	UNREFERENCED_PARAMETER(processId);
	UNREFERENCED_PARAMETER(info);

	threads.erase(threadId);
}

DWORD Coverage::OnException(DWORD processId, DWORD threadId, const EXCEPTION_DEBUG_INFO& info)
{
	Debugger::OnException(processId, threadId, info);

	const bool isBreakpoint = info.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT;

	if (info.dwFirstChance!=0 && isBreakpoint)
	{
		const auto address = GLib::Win::Detail::ConvertAddress(info.ExceptionRecord.ExceptionAddress);
		const auto it = addresses.find(address);
		if (it != addresses.end())
		{
			 const GLib::Win::Symbols::SymProcess & p = Symbols().GetProcess(processId);

			Address & a = it->second;
			p.Write(address, a.OldData());
			a.Visit();

			auto const tit = threads.find(threadId);
			if (tit == threads.end())
			{
				throw std::runtime_error("Thread not found");
			}

			CONTEXT ctx {};
			ctx.ContextFlags = CONTEXT_ALL; // NOLINT(hicpp-signed-bitwise) baad macro
			 GLib::Win::Util::AssertTrue(::GetThreadContext(tit->second, &ctx), "GetThreadContext");
#ifdef _WIN64
			--ctx.Rip;
#elif  _WIN32
			--ctx.Eip;
#endif
			 GLib::Win::Util::AssertTrue(::SetThreadContext(tit->second, &ctx), "SetThreadContext");
		}
		return DBG_CONTINUE;
	}

	return DBG_EXCEPTION_NOT_HANDLED;
}

void Coverage::CreateReport(unsigned int processId)
{
	const GLib::Win::Symbols::SymProcess & process = Symbols().GetProcess(processId);

	std::map<ULONG, Function> indexToFunction;

	for (const auto & a : addresses)
	{
		GLib::Win::Symbols::Symbol symbol = process.GetSymbolFromAddress(a.first);
		const Address & address = a.second;

		auto it = indexToFunction.find(symbol.Index);
		if (it == indexToFunction.end())
		{
			GLib::Win::Symbols::Symbol parent;
			process.TryGetClassParent(symbol, parent);

			// ok need to merge multiple hits from templates, but not overloads?
			// store namespaceName+className+functionName+isTemplate
			 // if not a template then generate additional inserts? template specialisations?
			std::string nameSpace;
			std::string typeName;
			std::string functionName;
			CleanupFunctionNames(symbol.name, parent.name, nameSpace, typeName, functionName);
			it = indexToFunction.emplace(symbol.Index, Function{ nameSpace, typeName, functionName }).first;
		}

		it->second.Accumulate(address);
	}

	// just do both for now
	CreateXmlReport(indexToFunction);
	CreateHtmlReport(indexToFunction, executable);
}

// move
void Coverage::CreateXmlReport(const std::map<ULONG, Function> & indexToFunction) const
{
	// merge templates here?
	// std::set<Function> nameToFunction;
	// for (const auto & p : indexToFunction)
	// {
	// 	const Function & function = p.second;
	// 	const auto & it = nameToFunction.find(function);
	// 	if (it == nameToFunction.end())
	// 	{
	// 		nameToFunction.insert(function);
	// 	}
	// 	else
	// 	{
	// 		it->Merge(p.second);
	// 	}
	// }

	size_t allLines{};
	size_t coveredLines{};
	for (const auto & x : indexToFunction)
	{
		allLines += x.second.AllLines();
		coveredLines += x.second.CoveredLines();
	}

	size_t fileId = 0;
	std::map<std::wstring, size_t> files;
	for (const auto & f : wideFiles)
	{
		files.emplace(f, fileId++);
	}

	XmlPrinter p;

	p.PushDeclaration();
	p.OpenElement("results");
	p.OpenElement("modules");
	p.OpenElement("module");
	p.PushAttribute("name", std::filesystem::path(GLib::Cvt::a2w(executable)).filename().u8string());
	p.PushAttribute("path", executable);

	p.PushAttribute("id", "0"); // todo, hash of exe?

	// report generator AVs without these, todo calculate them?
	p.PushAttribute("block_coverage", "0");
	p.PushAttribute("line_coverage", "0");
	p.PushAttribute("blocks_covered", "0");
	p.PushAttribute("blocks_not_covered", "0");

	p.PushAttribute("lines_covered", coveredLines);
	p.PushAttribute("lines_partially_covered", coveredLines); //?
	p.PushAttribute("lines_not_covered", allLines - coveredLines);

	p.OpenElement("functions");
	size_t functionId{};
	for (const auto & idFunctionPair : indexToFunction)
	{
		const Function & function = idFunctionPair.second;
		p.OpenElement("function");
		// id="3048656" name="TestCollision" namespace="Sat" type_name="" block_coverage="0.00" line_coverage="0.00" blocks_covered="0" blocks_not_covered="30" lines_covered="0" lines_partially_covered="0" lines_not_covered="20">
		p.PushAttribute("id", functionId++);
		p.PushAttribute("name", function.FunctionName());
		p.PushAttribute("namespace", function.NameSpace());
		p.PushAttribute("type_name", function.ClassName());

		// todo calculate these
		p.PushAttribute("block_coverage", "0");
		p.PushAttribute("line_coverage", "0");
		p.PushAttribute("blocks_covered", "0");
		p.PushAttribute("blocks_not_covered", "0");

		p.PushAttribute("lines_covered", function.CoveredLines());
		// todo p.PushAttribute("lines_partially_covered", "0");
		p.PushAttribute("lines_not_covered", function.AllLines() - function.CoveredLines());

		p.OpenElement("ranges");

		for (const auto & fileLines : function.FileLines())
		{
			// <range source_id = "23" covered = "yes" start_line = "27" start_column = "0" end_line = "27" end_column = "0" / >
			const std::wstring & fileName = fileLines.first;
			const auto & lines = fileLines.second;
			const size_t sourceId = files.find(fileName)->second; // check

			for (const auto & line : lines)
			{
				const unsigned lineNumber = line.first;
				const bool covered = line.second;

				p.OpenElement("range");
				
				p.PushAttribute("source_id", sourceId);
				p.PushAttribute("covered", covered ? "yes" : "no");
				p.PushAttribute("start_line", lineNumber);
				// todo? p.PushAttribute("start_column", "0");
				p.PushAttribute("end_line", lineNumber);
				// todo ?p.PushAttribute("end_column", "0");
				p.CloseElement(); // range
			}
		}
		p.CloseElement(); // ranges
		p.CloseElement(); // function
	}
	p.CloseElement(); // functions

	p.OpenElement("source_files");

	for (const auto & file : files)
	{
		p.OpenElement("source_file");
		p.PushAttribute("id", file.second);
		p.PushAttribute("path", GLib::Cvt::w2a(file.first));
		// todo? https://stackoverflow.com/questions/13256446/compute-md5-hash-value-by-c-winapi
		//p.PushAttribute("checksum_type", "MD5");
		//p.PushAttribute("checksum", "blah");
		p.CloseElement(); // source_file
	}
	p.Close();

	create_directories(reportPath);
	std::ofstream fs(reportPath / "Coverage.xml");
	if(!fs)
	{
		throw std::runtime_error("Unable to create file");
	}
	fs << p.Xml();
}

void Coverage::CreateHtmlReport(const std::map<ULONG, Function> & indexToFunctionMap, const std::string & title) const
{
	HtmlReport report(title, reportPath / "HtmlReport", ConvertFunctionDataToFileData(indexToFunctionMap));
	(void)report; // class with no methods :(
}

std::map<std::filesystem::path, FileCoverageData> Coverage::ConvertFunctionDataToFileData(const std::map<ULONG, Function> & indexToFunctionMap)
{
	CaseInsensitiveMap<wchar_t, std::multimap<unsigned int, Function>> fileNameToFunctionMap; // just use map<path..>?

	for (const auto & it : indexToFunctionMap)
	{
		const Function & function = it.second;
		for (const auto & fileLineIt : function.FileLines())
		{
			const std::wstring & fileName = fileLineIt.first;
			const std::map<unsigned, bool> & lineCoverage = fileLineIt.second;
			unsigned int startLine = lineCoverage.begin()->first;
			fileNameToFunctionMap[fileName].emplace(startLine, function);
		}
	}

	std::map<std::filesystem::path, FileCoverageData> fileCoverageData;

	for (const auto & fd : fileNameToFunctionMap)
	{
		const std::filesystem::path & filePath = fd.first;
		const std::multimap<unsigned, Function> & startLineToFunctionMap = fd.second;

		auto fileIt = fileCoverageData.find(filePath);
		if (fileIt == fileCoverageData.end())
		{
			fileIt = fileCoverageData.emplace(filePath, filePath).first;
		}

		FileCoverageData & coverageData = fileIt->second;

		for (const auto & it : startLineToFunctionMap)
		{
			const Function & function = it.second;
			const FileLines & fileLines = function.FileLines();

			auto justFileNameIt = fileLines.find(filePath.wstring());
			if (justFileNameIt == fileLines.end())
			{
				continue;
			}

			for (const auto & lineHitPair : justFileNameIt->second)
			{
				coverageData.AddLine(lineHitPair.first, lineHitPair.second);
			}
		}
	}

	return fileCoverageData;
}

void Coverage::Delaminate(std::string & name)
{
	for (size_t pos = name.find("<lambda"); pos != std::string::npos; pos = name.find("<lambda", pos))
	{
		name.erase(pos, 1);
		pos = name.find('>', pos);
		if (pos != std::string::npos)
		{
			name.erase(pos, 1);
		}
	}
}

void Coverage::CleanupFunctionNames(const std::string & name, const std::string & typeName,
	std::string & className, std::string & functionName, std::string & nameSpace) const
{
	className = typeName;
	functionName = name;
	Delaminate(className);
	Delaminate(functionName);

	std::smatch m;
	if (!className.empty())
	{
		std::regex_search(className, m, nameSpaceRegex);
		if (!m.empty())
		{
			size_t len = m[0].str().size();
			if (len >= 2)
			{
				nameSpace = className.substr(0, len - 2);

				if (className.compare(0, nameSpace.size(), nameSpace) == 0)
				{
					className.erase(0, len);
				}
				if (functionName.compare(0, nameSpace.size(), nameSpace) == 0)
				{
					functionName.erase(0, len);
				}
			}
			if (functionName.compare(0, className.size(), className) == 0)
			{
				functionName.erase(0, className.size() + 2);
			}
		}
	}
	else
	{
		std::regex_search(functionName, m, nameSpaceRegex);
		if (!m.empty())
		{
			size_t len = m[0].str().size();
			if (len >= 2)
			{
				nameSpace = functionName.substr(0, len - 2);
				functionName.erase(0, len);
			}
			className = "<Functions>"; // avoid blank line in ReportGenerator
		}
	}
}