using Mesen.Config;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Mesen.Utilities
{
	//The built-in YuXing host's texts as one file anyone can edit: YuxingServer.txt in Mesen's home
	//folder. The first call writes the built-in texts there, and every call reads them back, so an
	//edit shows on the next dial, and deleting the file brings the built-in texts back. It is UTF-8
	//(or GB2312, as a text editor may save it); this turns it into what the core wants: GB2312,
	//wrapped to the program's 30 columns (see YuxingServerData.h).
	public static class YuxingServerFile
	{
		//What the host asks for with a YuxingWebRequest when a call connects
		public const string DataRequest = "yuxing:data";

		private const int Columns = 30;

		//网友天地's forums, in the program's order
		private static readonly string[] Forums = {
			"广而告之", "新手加油站", "英语沙龙", "非常校园", "夕阳红", "体育", "新干线", "游戏迷俱乐部", "健康乐园",
			"股市风云", "网络作家", "天地行"
		};

		private const string DefaultHome = "裕兴浏览器 主页\n" +
			"这是 MesenCE 模拟器内置的主页。当年的裕兴服务器早已关闭，这里的网页是模拟器从今天的互联网上取来，改成文字显示的。\n" +
			"\n" +
			"操作：< > 键选择链接，回车打开，ESC 返回上一页，空格键翻页，g 键输入网址，m 键回主页。\n" +
			"\n" +
			"可以试试这些网站：\n" +
			"[维基百科（中文）](https://zh.m.wikipedia.org/zh-cn/Wikipedia:%E9%A6%96%E9%A1%B5)\n" +
			"[维基百科随机条目](https://zh.m.wikipedia.org/zh-cn/Special:%E9%9A%8F%E6%9C%BA%E9%A1%B5%E9%9D%A2)\n" +
			"[Hacker News](https://news.ycombinator.com/)\n" +
			"[NPR News (text)](https://text.npr.org/)\n" +
			"[CNN Lite](https://lite.cnn.com/)\n" +
			"[Lynx](https://lynx.invisible-island.net/)";

		//Everything before the first entry is for reading. An example of an entry here must not
		//start a line with "===", so these are indented
		private const string Header =
			"裕兴上网服务的内容 / What the built-in YuXing online server serves\n" +
			"\n" +
			"这个文件可以随意修改，下次拨号时生效。删除这个文件，下次拨号时会恢复内置内容。\n" +
			"文件用 UTF-8 保存（GB2312 也可以）。文字会自动按屏幕宽度（30 列，一个汉字占 2 列）换行。\n" +
			"注意：改过内容后，旧的录像可能无法正常回放。\n" +
			"\n" +
			"Edit anything here; it takes effect on the next dial. Delete this file to get the built-in\n" +
			"texts back on the next dial. It is UTF-8 (GB2312 works too). Text wraps by itself to the\n" +
			"screen's 30 columns (a Chinese character takes 2). Recordings made with other content may\n" +
			"not replay the same.\n" +
			"\n" +
			"Each entry starts with a line beginning === at the very start of the line, and runs to the\n" +
			"next one. Entries can be in any order; add or remove as you like.\n" +
			"\n" +
			"  === file <directory> <name> | <title>\n" +
			"      A file, and the title shown for it in its list.\n" +
			"      16        裕兴世界's boards. The program asks for these names: ggl.txt 公告栏,\n" +
			"                klyb.txt 留言板, dnxx.txt 电脑学校, tsg.txt 图书馆, yxcs.txt 裕兴超市.\n" +
			"                股市传真 asks here too, for names such as show0910.txt (沪市 09:10).\n" +
			"      1 to 9    软件下载's nine libraries. Anything listed here can be downloaded to a\n" +
			"                floppy. Use short ASCII names.\n" +
			"      17 to 22  电脑学校's sections: 学习导航 同步课程 网上考场 资料荟萃 知识宝典 心理医生.\n" +
			"      0         none.txt, the page shown for anything the server does not have.\n" +
			"\n" +
			"  === file <directory> <name> | <title> | <path>\n" +
			"      A file sent exactly as it is on your disk, for what is not text (a game for the\n" +
			"      libraries, say). The entry has no text; a path that is not full starts at this\n" +
			"      file's folder.\n" +
			"\n" +
			"  === forum <number> <name>\n" +
			"      A thread of 网友天地: forum 1 to 12 as listed below. Lines \"Subject:\" and \"Author:\", an\n" +
			"      empty line, then the post. A forum with no thread keeps its built-in one. Subjects\n" +
			"      are cut at 10 Chinese characters, authors at 15.\n" +
			"\n" +
			"  === mail\n" +
			"      A letter of 电子邮件. Lines \"From:\" and \"Subject:\", an empty line, then the letter.\n" +
			"\n" +
			"  === home\n" +
			"      The browser's home page. First line the title, an empty line a new paragraph,\n" +
			"      [text](address) a link.\n" +
			"\n" +
			"-----------------------------------------------------------------------------------------\n" +
			"\n";

		private static string TextFile { get { return Path.Combine(ConfigManager.HomeFolder, "YuxingServer.txt"); } }

		private static readonly Encoding Gb = GetGb();

		private static Encoding GetGb()
		{
			Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
			return Encoding.GetEncoding(936);
		}

		private record Entry(string Kind, string Argument, string Text);

		//On the emulation thread, while the host waits
		public static void Supply()
		{
			try {
				if(!File.Exists(TextFile)) {
					Export();
				}
				byte[] data = Build(ReadEntries());
				EmuApi.YuxingServerSetData(data, (uint)data.Length);
			} catch(Exception) {
				//A file that cannot be read must not stop the call
				EmuApi.YuxingServerSetData(null, 0);
			}
		}

		public static string HomeText()
		{
			try {
				if(File.Exists(TextFile)) {
					Entry? home = ReadEntries().LastOrDefault(e => e.Kind == "home");
					if(home != null && home.Text.Trim().Length > 0) {
						return home.Text.TrimStart('\n');
					}
				}
			} catch(Exception) {
			}
			return DefaultHome;
		}

		private static string ReadText(string path)
		{
			byte[] data = File.ReadAllBytes(path);
			try {
				return new UTF8Encoding(false, true).GetString(data).TrimStart('\uFEFF');
			} catch(ArgumentException) {
				return Gb.GetString(data);
			}
		}

		//An entry's text ends with its last line's end and the empty line that sets it off from the
		//next: both come off, and what was written is what is read
		private static List<Entry> ReadEntries()
		{
			string text = ReadText(TextFile).Replace("\r\n", "\n");
			List<Entry> entries = new();
			MatchCollection marks = Regex.Matches(text, @"^=== *(file|forum|mail|home)\b(.*)$", RegexOptions.Multiline);
			for(int i = 0; i < marks.Count; i++) {
				int start = marks[i].Index + marks[i].Length + 1;
				int end = i + 1 < marks.Count ? marks[i + 1].Index : text.Length;
				string body = start < end ? text.Substring(start, end - start) : "";
				for(int k = 0; k < 2 && body.EndsWith('\n'); k++) {
					body = body.Substring(0, body.Length - 1);
				}
				entries.Add(new Entry(marks[i].Groups[1].Value, marks[i].Groups[2].Value.Trim(), body));
			}
			return entries;
		}

		private class Reader
		{
			private readonly byte[] _data;
			private int _pos;

			public Reader(byte[] data) { _data = data; }

			public int Number()
			{
				int value = BitConverter.ToInt32(_data, _pos);
				_pos += 4;
				return value;
			}

			public byte[] Blob()
			{
				int size = Number();
				byte[] value = new byte[size];
				Array.Copy(_data, _pos, value, 0, size);
				_pos += size;
				return value;
			}

			public string Text() { return Gb.GetString(Blob()); }
		}

		//The built-in texts as the file
		private static void Export()
		{
			byte[] data = new byte[EmuApi.YuxingServerGetDefaults(null, 0)];
			EmuApi.YuxingServerGetDefaults(data, (uint)data.Length);
			Reader reader = new(data);
			StringBuilder text = new(Header);

			int sections = reader.Number();
			for(int i = 0; i < sections; i++) {
				int dir = reader.Number();
				int files = reader.Number();
				for(int k = 0; k < files; k++) {
					string name = reader.Text();
					string title = reader.Text().TrimEnd();
					byte[] content = reader.Blob();
					if(IsLibrary(dir)) {
						//The size at the title's end is worked out again
						title = title.Substring(0, title.LastIndexOf(' ')).TrimEnd();
					}
					int start = HasHeader(dir) ? 8 : 0;
					int end = content.Length > start && content[^1] == 0x1A ? content.Length - 1 : content.Length;
					text.Append("=== file " + dir + " " + name + " | " + title + "\n");
					text.Append(Gb.GetString(content, start, end - start).Replace("\r\n", "\n") + "\n\n");
				}
			}

			int forums = reader.Number();
			for(int i = 0; i < forums; i++) {
				int threads = reader.Number();
				for(int k = 0; k < threads; k++) {
					string subject = reader.Text();
					string author = reader.Text();
					string body = reader.Text().Replace("\r\n", "\n");
					text.Append("=== forum " + (i + 1) + " " + Forums[i] + "\n");
					text.Append("Subject: " + subject + "\nAuthor: " + author + "\n\n" + body + "\n\n");
				}
			}

			int mails = reader.Number();
			for(int i = 0; i < mails; i++) {
				string from = reader.Text();
				string subject = reader.Text();
				string body = reader.Text().Replace("\r\n", "\n");
				text.Append("=== mail\nFrom: " + from + "\nSubject: " + subject + "\n\n" + body + "\n\n");
			}

			text.Append("=== home\n" + DefaultHome + "\n\n");
			File.WriteAllText(TextFile, text.ToString().Replace("\n", "\r\n"), new UTF8Encoding(false));
		}

		private static bool IsLibrary(int dir) { return dir >= 1 && dir <= 9; }
		private static bool HasHeader(int dir) { return dir >= 17; }

		//The entries in the core's layout
		private static byte[] Build(List<Entry> entries)
		{
			SortedDictionary<int, List<(byte[] Name, byte[] Title, byte[] Data)>> sections = new();
			List<(byte[] Subject, byte[] Author, byte[] Body)>[] forums = new List<(byte[], byte[], byte[])>[Forums.Length];
			for(int i = 0; i < forums.Length; i++) {
				forums[i] = new();
			}
			List<(byte[] From, byte[] Subject, byte[] Body)> mails = new();

			foreach(Entry entry in entries) {
				if(entry.Kind == "file") {
					string[] parts = entry.Argument.Split('|').Select(p => p.Trim()).ToArray();
					string[] place = parts[0].Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
					if(place.Length < 2 || !int.TryParse(place[0], out int dir) || dir < 0 || dir > 22) {
						continue;
					}
					byte[]? content = FileData(entry, parts, dir);
					if(content == null) {
						continue;
					}
					if(!sections.ContainsKey(dir)) {
						sections[dir] = new();
					}
					string title = parts.Length > 1 && parts[1].Length > 0 ? parts[1] : place[1];
					sections[dir].Add((GbBytes(place[1]), TitleColumn(dir, title, content.Length), content));
				} else if(entry.Kind == "forum") {
					string number = entry.Argument.Split(' ')[0];
					if(int.TryParse(number, out int forum) && forum >= 1 && forum <= Forums.Length) {
						(Dictionary<string, string> head, string body) = ReadLetter(entry.Text);
						//The subject goes to the program as hex in a field of 40 digits
						forums[forum - 1].Add((
							Limit(GbBytes(head.GetValueOrDefault("subject", "")), 20),
							Limit(GbBytes(head.GetValueOrDefault("author", "")), 30),
							GbLines(Wrap(body))
						));
					}
				} else if(entry.Kind == "mail") {
					(Dictionary<string, string> head, string body) = ReadLetter(entry.Text);
					mails.Add((
						GbBytes(head.GetValueOrDefault("from", "user@yuxing.cn.net")),
						GbBytes(head.GetValueOrDefault("subject", "")),
						GbLines(Wrap(body))
					));
				}
			}

			using MemoryStream stream = new();
			using BinaryWriter writer = new(stream);
			writer.Write(sections.Count);
			foreach(KeyValuePair<int, List<(byte[] Name, byte[] Title, byte[] Data)>> section in sections) {
				writer.Write(section.Key);
				writer.Write(section.Value.Count);
				foreach((byte[] name, byte[] title, byte[] content) in section.Value) {
					Put(writer, name);
					Put(writer, title);
					Put(writer, content);
				}
			}
			writer.Write(forums.Length);
			foreach(List<(byte[] Subject, byte[] Author, byte[] Body)> forum in forums) {
				writer.Write(forum.Count);
				foreach((byte[] subject, byte[] author, byte[] body) in forum) {
					Put(writer, subject);
					Put(writer, author);
					Put(writer, body);
				}
			}
			writer.Write(mails.Count);
			foreach((byte[] from, byte[] subject, byte[] body) in mails) {
				Put(writer, from);
				Put(writer, subject);
				Put(writer, body);
			}
			writer.Flush();
			return stream.ToArray();
		}

		private static void Put(BinaryWriter writer, byte[] data)
		{
			writer.Write(data.Length);
			writer.Write(data);
		}

		//"Name: value" lines, an empty line, the rest
		private static (Dictionary<string, string>, string) ReadLetter(string text)
		{
			Dictionary<string, string> head = new();
			int pos = 0;
			while(pos < text.Length) {
				int end = text.IndexOf('\n', pos);
				if(end < 0) {
					end = text.Length;
				}
				string line = text.Substring(pos, end - pos);
				pos = Math.Min(end + 1, text.Length);
				if(line.Length == 0) {
					break;
				}
				int colon = line.IndexOf(':');
				if(colon > 0) {
					head[line.Substring(0, colon).Trim().ToLowerInvariant()] = line.Substring(colon + 1).Trim();
				}
			}
			return (head, text.Substring(pos));
		}

		//The entry's text, or the file its third part names, as it is. A file that is not there
		//leaves the entry out
		private static byte[]? FileData(Entry entry, string[] parts, int dir)
		{
			if(parts.Length > 2 && parts[2].Length > 0) {
				string path = Path.Combine(ConfigManager.HomeFolder, parts[2]);
				return File.Exists(path) ? File.ReadAllBytes(path) : null;
			}
			List<byte> data = new();
			if(HasHeader(dir)) {
				data.AddRange(new byte[] { 0, 0, 0, 0, 0x08, 0, 0, 0 });
			}
			data.AddRange(GbLines(Wrap(entry.Text)));
			data.Add(0x1A);
			return data.ToArray();
		}

		//30 bytes: the title, and for a library the size at the right
		private static byte[] TitleColumn(int dir, string title, int size)
		{
			byte[] text = GbBytes(title);
			byte[] column = Enumerable.Repeat((byte)' ', Columns).ToArray();
			byte[] kb = IsLibrary(dir) ? GbBytes(Math.Max(1, (size + 1023) / 1024) + "K") : Array.Empty<byte>();
			text = Limit(text, Columns - (kb.Length > 0 ? kb.Length + 1 : 0));
			Array.Copy(text, column, text.Length);
			Array.Copy(kb, 0, column, Columns - kb.Length, kb.Length);
			return column;
		}

		private static byte[] GbBytes(string text)
		{
			List<byte> bytes = new();
			foreach(char c in text) {
				bytes.AddRange(YuxingWebService.ToGb(c));
			}
			return bytes.ToArray();
		}

		private static byte[] GbLines(string text)
		{
			List<byte> bytes = new();
			string[] lines = text.Split('\n');
			for(int i = 0; i < lines.Length; i++) {
				bytes.AddRange(GbBytes(lines[i]));
				if(i < lines.Length - 1) {
					bytes.Add(0x0D);
					bytes.Add(0x0A);
				}
			}
			return bytes.ToArray();
		}

		//Cut at a character's edge
		private static byte[] Limit(byte[] text, int size)
		{
			if(text.Length <= size) {
				return text;
			}
			int end = 0;
			while(end < size) {
				int next = end + (text[end] >= 0xA1 ? 2 : 1);
				if(next > size) {
					break;
				}
				end = next;
			}
			return text.Take(end).ToArray();
		}

		private static int Width(char c) { return c > 0x7F ? 2 : 1; }

		//To the program's 30 columns, as YuxingWebService lays out a page: no row starts with
		//closing punctuation, the character before it comes down too. A wrapped row keeps the
		//paragraph's indent
		private static string Wrap(string text)
		{
			List<string> rows = new();
			foreach(string para in text.Replace("\r\n", "\n").Split('\n')) {
				int indent = para.Length - para.TrimStart(' ').Length;
				StringBuilder row = new();
				int width = 0;
				foreach(char c in para) {
					int w = Width(c);
					if(width + w > Columns) {
						if(YuxingWebService.IsClosing(c) && row.Length > 0 && row[^1] > 0x7F) {
							char last = row[^1];
							row.Length--;
							rows.Add(row.ToString());
							row.Clear();
							row.Append(' ', indent).Append(last);
							width = indent + 2;
						} else {
							rows.Add(row.ToString());
							row.Clear();
							row.Append(' ', indent);
							width = indent;
						}
					}
					row.Append(c);
					width += w;
				}
				rows.Add(row.ToString());
			}
			return string.Join("\n", rows);
		}
	}
}
