#pragma once

#include "GLib/XmlPrinter.h"

#include <filesystem>

// strict XHTML? no longer used
class HtmlPrinter : public Xml::Printer
{
public:
	HtmlPrinter(const std::string & title, const std::filesystem::path & css)
		: Xml::Printer(true)
	{
		PushDocType("html");
		OpenElement("html");
		OpenElement("head");
		OpenElement("meta");
		PushAttribute("charset", "UTF-8");
		OpenElement("title");
		PushText(title);
		CloseElement(); // title
		CloseElement(); // meta
		CloseElement(); // head

		if (!css.empty())
		{
			OpenElement("link");
			PushAttribute("rel", "stylesheet");
			PushAttribute("type", "text/css");
			PushAttribute("href", css.generic_u8string());
			CloseElement(); // link
		}
		CloseElement(); // head
		OpenElement("body");
	}

	void Anchor(const std::filesystem::path & path, const std::string & text)
	{
		OpenElement("a", false);
		PushAttribute("href", path.generic_u8string());
		PushText(text);
		CloseElement(false);
	}

	void Span(const std::string & text, const std::string & cls)
	{
		OpenElement("span", false);
		PushAttribute("class", cls);
		PushText(text);
		CloseElement(false);
	}

	void OpenTable(int padding=0, int spacing=0, int border=0)
	{
		OpenElement("table");
		PushAttribute("cellpadding", std::to_string(padding));
		PushAttribute("cellspacing", std::to_string(spacing));
		PushAttribute("border", std::to_string(border));
	}

	void LineBreak(const char * element = "hr")
	{
		OpenElement(element);
		CloseElement(true);
	}
};
