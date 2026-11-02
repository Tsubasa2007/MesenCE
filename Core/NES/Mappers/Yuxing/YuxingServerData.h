#pragma once
#include "pch.h"
#include <memory>
#include <mutex>
#include "NES/Mappers/Yuxing/YuxingHostContent.h"

//What the built-in YuXing host serves, in GB2312. It starts as the texts built into the emulator
//(YuxingHostContent.h); the UI keeps them as an editable file in the home folder and, when a call
//connects, hands them back through here in the layout below. Only the UI can reach the file, and
//only it knows UTF-8 and how the program wraps text. A host holds on to what it got for the
//length of its call.
//
//The layout, little-endian, a "blob" being a 32-bit length and its bytes:
//  count, then per section: directory, count, then per file: blob name, blob title, blob data
//  count, then per forum: count, then per thread: blob subject, blob author, blob body
//  count, then per letter: blob sender, blob subject, blob body
struct YuxingDataFile
{
	string Name;
	vector<uint8_t> Title;
	vector<uint8_t> Data;
};

struct YuxingDataSection
{
	uint8_t Dir = 0;
	vector<YuxingDataFile> Files;
};

struct YuxingDataThread
{
	vector<uint8_t> Subject;
	vector<uint8_t> Author;
	vector<uint8_t> Body;
};

struct YuxingDataForum
{
	vector<YuxingDataThread> Threads;
};

struct YuxingDataMail
{
	string From;
	vector<uint8_t> Subject;
	vector<uint8_t> Body;
};

struct YuxingContent
{
	vector<YuxingDataSection> Sections;
	vector<YuxingDataForum> Forums;
	vector<YuxingDataMail> Mails;
};

class YuxingServerData
{
private:
	inline static std::mutex _lock;
	inline static shared_ptr<const YuxingContent> _current;

	static vector<uint8_t> Bytes(const uint8_t* data, uint32_t size)
	{
		return vector<uint8_t>(data, data + size);
	}

	static shared_ptr<YuxingContent> Defaults()
	{
		shared_ptr<YuxingContent> content = std::make_shared<YuxingContent>();
		for(uint32_t i = 0; i < YuxingHostSectionCount; i++) {
			const YuxingHostSection& section = YuxingHostSections[i];
			YuxingDataSection out;
			out.Dir = section.Dir;
			for(uint32_t k = 0; k < section.Count; k++) {
				const YuxingHostFile& file = section.Files[k];
				out.Files.push_back({ file.Name, Bytes(file.Title, 30), Bytes(file.Data, file.Size) });
			}
			content->Sections.push_back(std::move(out));
		}
		for(uint32_t i = 0; i < YuxingTinForumCount; i++) {
			const YuxingTinForum& forum = YuxingTinForums[i];
			YuxingDataForum out;
			for(uint32_t k = 0; k < forum.Count; k++) {
				const YuxingTinThread& thread = forum.Threads[k];
				out.Threads.push_back({
					Bytes(thread.Subject, thread.SubjectSize),
					Bytes(thread.Author, thread.AuthorSize),
					Bytes(thread.Body, thread.BodySize)
				});
			}
			content->Forums.push_back(std::move(out));
		}
		for(uint32_t i = 0; i < YuxingMailCount; i++) {
			const YuxingMail& mail = YuxingMails[i];
			content->Mails.push_back({
				mail.From, Bytes(mail.Subject, mail.SubjectSize), Bytes(mail.Body, mail.BodySize)
			});
		}
		return content;
	}

	static void PutNumber(vector<uint8_t>& out, uint32_t value)
	{
		for(int i = 0; i < 4; i++) {
			out.push_back((uint8_t)(value >> (i * 8)));
		}
	}

	static void PutBlob(vector<uint8_t>& out, const uint8_t* data, size_t size)
	{
		PutNumber(out, (uint32_t)size);
		out.insert(out.end(), data, data + size);
	}

	static void PutBlob(vector<uint8_t>& out, const vector<uint8_t>& data) { PutBlob(out, data.data(), data.size()); }
	static void PutBlob(vector<uint8_t>& out, const string& data) { PutBlob(out, (const uint8_t*)data.data(), data.size()); }

	static vector<uint8_t> Flatten(const YuxingContent& content)
	{
		vector<uint8_t> out;
		PutNumber(out, (uint32_t)content.Sections.size());
		for(const YuxingDataSection& section : content.Sections) {
			PutNumber(out, section.Dir);
			PutNumber(out, (uint32_t)section.Files.size());
			for(const YuxingDataFile& file : section.Files) {
				PutBlob(out, file.Name);
				PutBlob(out, file.Title);
				PutBlob(out, file.Data);
			}
		}
		PutNumber(out, (uint32_t)content.Forums.size());
		for(const YuxingDataForum& forum : content.Forums) {
			PutNumber(out, (uint32_t)forum.Threads.size());
			for(const YuxingDataThread& thread : forum.Threads) {
				PutBlob(out, thread.Subject);
				PutBlob(out, thread.Author);
				PutBlob(out, thread.Body);
			}
		}
		PutNumber(out, (uint32_t)content.Mails.size());
		for(const YuxingDataMail& mail : content.Mails) {
			PutBlob(out, mail.From);
			PutBlob(out, mail.Subject);
			PutBlob(out, mail.Body);
		}
		return out;
	}

	class Reader
	{
	private:
		const uint8_t* _data;
		uint32_t _length;
		uint32_t _pos = 0;

	public:
		bool Failed = false;

		Reader(const uint8_t* data, uint32_t length) : _data(data), _length(length) {}

		uint32_t Number()
		{
			if(_length - _pos < 4) {
				Failed = true;
				return 0;
			}
			uint32_t value = _data[_pos] | (_data[_pos + 1] << 8) | (_data[_pos + 2] << 16) | ((uint32_t)_data[_pos + 3] << 24);
			_pos += 4;
			return value;
		}

		vector<uint8_t> Blob()
		{
			uint32_t size = Number();
			if(Failed || size > _length - _pos) {
				Failed = true;
				return {};
			}
			vector<uint8_t> value(_data + _pos, _data + _pos + size);
			_pos += size;
			return value;
		}

		string Text()
		{
			vector<uint8_t> value = Blob();
			return string(value.begin(), value.end());
		}

		//A count that cannot be right stops the reading, not the allocation
		uint32_t Count()
		{
			uint32_t value = Number();
			if(value > _length) {
				Failed = true;
				return 0;
			}
			return value;
		}
	};

	static shared_ptr<YuxingContent> Unflatten(const uint8_t* data, uint32_t length)
	{
		Reader reader(data, length);
		shared_ptr<YuxingContent> content = Defaults();
		content->Sections.clear();
		uint32_t sections = reader.Count();
		for(uint32_t i = 0; i < sections && !reader.Failed; i++) {
			YuxingDataSection section;
			section.Dir = (uint8_t)reader.Number();
			uint32_t files = reader.Count();
			for(uint32_t k = 0; k < files && !reader.Failed; k++) {
				YuxingDataFile file;
				file.Name = reader.Text();
				file.Title = reader.Blob();
				file.Data = reader.Blob();
				section.Files.push_back(std::move(file));
			}
			content->Sections.push_back(std::move(section));
		}
		//The program takes a forum with no threads for a full page of rubbish, so one left empty
		//keeps the built-in thread, and there are always its twelve
		uint32_t forums = reader.Count();
		for(uint32_t i = 0; i < forums && !reader.Failed; i++) {
			YuxingDataForum forum;
			uint32_t threads = reader.Count();
			for(uint32_t k = 0; k < threads && !reader.Failed; k++) {
				YuxingDataThread thread;
				thread.Subject = reader.Blob();
				thread.Author = reader.Blob();
				thread.Body = reader.Blob();
				forum.Threads.push_back(std::move(thread));
			}
			if(i < content->Forums.size() && !forum.Threads.empty()) {
				content->Forums[i] = std::move(forum);
			}
		}
		content->Mails.clear();
		uint32_t mails = reader.Count();
		for(uint32_t i = 0; i < mails && !reader.Failed; i++) {
			YuxingDataMail mail;
			mail.From = reader.Text();
			mail.Subject = reader.Blob();
			mail.Body = reader.Blob();
			content->Mails.push_back(std::move(mail));
		}
		return reader.Failed ? Defaults() : content;
	}

public:
	static shared_ptr<const YuxingContent> Get()
	{
		std::lock_guard<std::mutex> lock(_lock);
		if(!_current) {
			_current = Defaults();
		}
		return _current;
	}

	//From the UI. Nothing, or anything that does not read, means the built-in texts
	static void Set(const uint8_t* data, uint32_t length)
	{
		shared_ptr<YuxingContent> content = data && length ? Unflatten(data, length) : Defaults();
		std::lock_guard<std::mutex> lock(_lock);
		_current = content;
	}

	//The built-in texts for the UI to write out: the size it takes, filled in if it fits
	static uint32_t GetDefaults(uint8_t* buffer, uint32_t size)
	{
		vector<uint8_t> out = Flatten(*Defaults());
		if(buffer && size >= out.size()) {
			memcpy(buffer, out.data(), out.size());
		}
		return (uint32_t)out.size();
	}
};
