#pragma once

#include <ostream>

namespace GLib
{
	namespace Util
	{
		template <typename T, class BufferType>
		class GenericOutStream : public std::basic_ostream<T>
		{
			typedef std::basic_ostream<T> Base;

			BufferType buffer;

		public:
			GenericOutStream(std::ios_base::fmtflags f = std::ios_base::fmtflags()) : Base(&buffer)
			{
				Base::setf(f);
			}

			const BufferType & Buffer()
			{
				return buffer;
			}
		};
	}
}