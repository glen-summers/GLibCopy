#pragma once

#include <GLib/Win/ComPtr.h>

#ifdef SIMPLECOM_LOG_QI_MISS
#include <GLib/Win/DebugWrite.h>
#include <GLib/Win/Uuid.h>
#include <GLib/Win/Registry.h>
#endif

#include <GLib/TypePredicates.h>

#include <atomic>

#define WRAP(x) x												 // NOLINT(cppcoreguidelines-macro-usage)
#define GLIB_COM_RULE_OF_FIVE(ClassName) /* NOLINT(cppcoreguidelines-macro-usage)*/                                                        \
public:                                                                                                                                    \
	(ClassName)() = default;                                                                                                                 \
	(ClassName)(const WRAP(ClassName) & other) = delete;                                                                                     \
	(ClassName)(WRAP(ClassName) && other) noexcept = delete;                                                                                 \
	WRAP(ClassName) & operator=(const WRAP(ClassName) & other) = delete;                                                                     \
	WRAP(ClassName) & operator=(WRAP(ClassName) && other) noexcept = delete;                                                                 \
                                                                                                                                           \
protected:                                                                                                                                 \
	virtual ~ClassName() = default;

namespace GLib::Win
{
	template <typename T>
	class ComPtrBase;

	template <typename T>
	class ComPtr;

	namespace Detail
	{
		// specialise this to avoid ambiguous casts to base interfaces
		template <typename I, typename T>
		I * Cast(T * t)
		{
			return static_cast<I *>(t);
		}

		template <typename F, typename... R>
		struct First
		{
			using Value = F;
		};

		template <typename T, typename Last>
		HRESULT Qi(T * t, const IID & iid, void ** ppvObject)
		{
			if (iid == __uuidof(Last))
			{
				auto * i = Cast<Last>(t);
				i->AddRef();
				*ppvObject = i;
				return S_OK;
			}
#ifdef SIMPLECOM_LOG_QI_MISS
			std::string itf = "Interface\\" + to_string(Util::Uuid(iid)), name;
			if (RegistryKeys::ClassesRoot.KeyExists(itf))
			{
				name = RegistryKeys::ClassesRoot.OpenSubKey(itf).GetString("");
			}
			Debug::Write("QI miss {0} : {1} {2}", typeid(T).name(), itf, name);
#else
			(void) iid;
#endif
			*ppvObject = nullptr;
			return E_NOINTERFACE;
		}

		template <typename T, typename First, typename Second, typename... Rest>
		HRESULT Qi(T * t, const IID & iid, void ** ppvObject)
		{
			if (iid == __uuidof(First)) // just call above?
			{
				auto * i = Cast<First>(t);
				i->AddRef();
				*ppvObject = i;
				return S_OK;
			}
			return Qi<T, Second, Rest...>(t, iid, ppvObject);
		}

		template <typename Interfaces>
		struct __declspec(novtable) Implements;

		template <typename... Interfaces>
		struct __declspec(novtable) Implements<GLib::Util::TypeList<Interfaces...>> : Interfaces...
		{};
	}

	template <typename T, typename... Interfaces>
	class Unknown
		: public Detail::Implements<typename GLib::Util::SelfTypeFilter<GLib::TypePredicates::HasNoInheritor, Interfaces...>::TupleType::Type>
	{
		std::atomic<ULONG> ref = 1;

	public:
		using DefaultInterface = typename Detail::First<Interfaces...>::Value;
		using PtrType = ComPtr<DefaultInterface>;
		GLIB_COM_RULE_OF_FIVE(Unknown)

		HRESULT STDMETHODCALLTYPE QueryInterface(const IID & id, void ** ppvObject) override
		{
			if (ppvObject == nullptr)
			{
				return E_POINTER;
			}
			if (id == __uuidof(IUnknown))
			{
				auto i = static_cast<IUnknown *>(static_cast<DefaultInterface *>(this));
				i->AddRef();
				*ppvObject = i;
				return S_OK;
			}
			return Detail::Qi<T, Interfaces...>(static_cast<T *>(this), id, ppvObject);
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return ++ref;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG ret = --ref;
			if (ret == 0)
			{
				delete this;
			}
			return ret;
		}
	};
}