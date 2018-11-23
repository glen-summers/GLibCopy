#pragma once

#include <Windows.h>

#include "GLib/genericoutstream.h"
#include "GLib/vectorstreambuffer.h"
#include "GLib/cvt.h"

namespace GLib
{
	namespace Win
	{
		namespace Debug
		{
			namespace Detail
			{
				typedef GLib::Util::VectorStreamBuffer<char> Buffer;
				class DebugBuffer : public Buffer
				{
					int_type overflow(int_type c) override
					{
						c = Buffer::overflow(c);
						if (c == traits_type::to_char_type('\n'))
						{
							Write(Get());
						}
						return c;
					}

					void Write(const char* s)
					{
						::OutputDebugStringW(Cvt::a2w(s).c_str());
						Reset();
					}
				};

				using DebugStream = GLib::Util::GenericOutStream<char, DebugBuffer>;
			}

			inline std::ostream & Stream()
			{
				static thread_local Detail::DebugStream debugStream(std::ios_base::boolalpha);
				return debugStream;
			}

#ifdef FORMATTER_H
			template <typename... Ts>
			void Write(const char * format, Ts&&... ts)
			{
				Formatter::Format(Stream(), format, std::forward<Ts>(ts)...) << std::endl;
			}
#endif
		}
	}
}