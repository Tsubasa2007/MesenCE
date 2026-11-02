using Mesen.Interop;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Mesen.Utilities
{
	//Pages for the browser of the YuXing floppy model's online program, whose server the
	//core plays (YuxingHost). The core asks with a YuxingWebRequest notification; this fetches the
	//address, turns the page into the host's layout and hands it back with YuxingWebResponse. The
	//layout fits the program's window: rows of at most 30 columns of GB2312 text (a Chinese
	//character takes two), with each link in brackets and its row and column noted. Lines of the
	//answer: "T" title, "U" address, "P" a row, "K<row> <column> <address>" a link.
	public static class YuxingWebService
	{
		private const int Columns = 30;
		private const int MaxRows = 3000;
		private const int MaxBytes = 2 * 1024 * 1024;
		private const string HomeAddress = "www.yuxing.cn.net";

		private static NotificationListener? _listener;
		private static HttpClient? _client;
		private static Encoding? _gb;

		public static void Start()
		{
			if(_listener != null) {
				return;
			}
			Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
			_gb = Encoding.GetEncoding(936, EncoderFallback.ReplacementFallback, DecoderFallback.ReplacementFallback);
			HttpClientHandler handler = new() {
				AllowAutoRedirect = true,
				AutomaticDecompression = DecompressionMethods.All
			};
			_client = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(25) };
			_client.DefaultRequestHeaders.UserAgent.ParseAdd("Lynx/2.9.0 libwww-FM/2.14");
			_client.DefaultRequestHeaders.AcceptLanguage.ParseAdd("zh-CN,zh;q=0.9,en;q=0.6");
			_listener = new NotificationListener();
			_listener.OnNotification += OnNotification;
		}

		private static void OnNotification(NotificationEventArgs e)
		{
			if(e.NotificationType != ConsoleNotificationType.YuxingWebRequest) {
				return;
			}
			YuxingWebRequestParams request = Marshal.PtrToStructure<YuxingWebRequestParams>(e.Parameter);
			string url = Marshal.PtrToStringUTF8(request.Url) ?? "";
			uint id = request.Id;
			if(url == YuxingServerFile.DataRequest) {
				YuxingServerFile.Supply();
				return;
			}
			Task.Run(async () => {
				Page page;
				try {
					page = await Fetch(url);
				} catch(Exception ex) {
					page = ErrorPage(url, ex is TaskCanceledException ? "Timed out" : ex.Message);
				}
				byte[] data = page.Encode();
				EmuApi.YuxingWebResponse(id, data, (uint)data.Length);
			});
		}

		private static bool IsHome(string url)
		{
			string u = url.Trim().ToLowerInvariant();
			u = Regex.Replace(u, "^https?://", "").TrimEnd('/');
			return u == "" || u == HomeAddress || u == "yuxing.cn.net";
		}

		private static async Task<Page> Fetch(string url)
		{
			if(IsHome(url)) {
				return HomePage();
			}
			string address = url.Trim();
			if(!Regex.IsMatch(address, "^[a-zA-Z][a-zA-Z0-9+.-]*://")) {
				address = "http://" + address;
			}
			if(!Uri.TryCreate(address, UriKind.Absolute, out Uri? uri) || (uri.Scheme != "http" && uri.Scheme != "https")) {
				return ErrorPage(url, "Not a web address");
			}

			using HttpResponseMessage response = await _client!.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead);
			Uri final = response.RequestMessage?.RequestUri ?? uri;
			if(!response.IsSuccessStatusCode) {
				return ErrorPage(final.AbsoluteUri, "HTTP " + (int)response.StatusCode + " " + response.ReasonPhrase);
			}

			byte[] body = await ReadLimited(response);
			string mediaType = response.Content.Headers.ContentType?.MediaType ?? "text/html";
			string? charset = response.Content.Headers.ContentType?.CharSet;
			if(!mediaType.StartsWith("text/") && !mediaType.Contains("html") && !mediaType.Contains("xml")) {
				return ErrorPage(final.AbsoluteUri, "Not a page: " + mediaType);
			}
			string text = Decode(body, charset);
			if(mediaType == "text/plain") {
				Page plain = new(final.AbsoluteUri.ToString());
				plain.Title = final.Host;
				foreach(string line in text.Split('\n')) {
					plain.AddText(line);
					plain.Break();
				}
				return plain;
			}
			return HtmlToPage(text, final);
		}

		private static async Task<byte[]> ReadLimited(HttpResponseMessage response)
		{
			using System.IO.Stream stream = await response.Content.ReadAsStreamAsync();
			using System.IO.MemoryStream ms = new();
			byte[] buffer = new byte[16384];
			int read;
			while((read = await stream.ReadAsync(buffer)) > 0 && ms.Length < MaxBytes) {
				ms.Write(buffer, 0, read);
			}
			return ms.ToArray();
		}

		private static string Decode(byte[] body, string? charset)
		{
			if(string.IsNullOrWhiteSpace(charset)) {
				//<meta charset="..."> or <meta ... content="text/html; charset=...">
				string head = Encoding.ASCII.GetString(body, 0, Math.Min(body.Length, 4096));
				Match m = Regex.Match(head, "charset\\s*=\\s*[\"']?([A-Za-z0-9_\\-]+)", RegexOptions.IgnoreCase);
				if(m.Success) {
					charset = m.Groups[1].Value;
				}
			}
			Encoding encoding = Encoding.UTF8;
			if(!string.IsNullOrWhiteSpace(charset)) {
				try {
					string name = charset.Trim().ToLowerInvariant();
					//GB2312 pages are written in GBK in practice
					if(name == "gb2312" || name == "gbk" || name == "gb18030" || name == "x-gbk") {
						encoding = Encoding.GetEncoding(name == "gb18030" ? "gb18030" : "gbk");
					} else {
						encoding = Encoding.GetEncoding(name);
					}
				} catch {
					encoding = Encoding.UTF8;
				}
			}
			return encoding.GetString(body);
		}

		//Blocks that stand apart: a blank row before and after
		private static readonly HashSet<string> ParagraphTags = new() {
			"p", "h1", "h2", "h3", "h4", "h5", "h6", "table", "ul", "ol", "dl", "blockquote", "pre", "hr", "form"
		};

		private static readonly HashSet<string> BlockTags = new() {
			"p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "table", "ul", "ol", "dl", "dt", "dd",
			"section", "article", "header", "footer", "nav", "main", "aside", "blockquote", "pre", "form", "hr",
			"figure", "figcaption", "center", "address", "details", "summary", "option"
		};

		private static Page HtmlToPage(string html, Uri baseUri)
		{
			Page page = new(baseUri.AbsoluteUri);
			Match title = Regex.Match(html, "<title[^>]*>(.*?)</title>", RegexOptions.IgnoreCase | RegexOptions.Singleline);
			page.Title = title.Success ? Clean(WebUtility.HtmlDecode(title.Groups[1].Value)) : baseUri.Host;

			//Leave out what is not text
			html = Regex.Replace(html, "<!--.*?-->", " ", RegexOptions.Singleline);
			//<!doctype ...> and the like: not tags to the loop below, so they would come out as text
			html = Regex.Replace(html, "<![^>]*>", " ");
			html = Regex.Replace(html, "<(script|style|noscript|svg|head|template|iframe|select|button)\\b.*?</\\1\\s*>", " ", RegexOptions.IgnoreCase | RegexOptions.Singleline);

			int pos = 0;
			string? linkUrl = null;
			foreach(Match tag in Regex.Matches(html, "<(/?)([a-zA-Z][a-zA-Z0-9]*)([^>]*)>")) {
				page.AddText(WebUtility.HtmlDecode(html.Substring(pos, tag.Index - pos)), linkUrl);
				pos = tag.Index + tag.Length;
				bool closing = tag.Groups[1].Value == "/";
				string name = tag.Groups[2].Value.ToLowerInvariant();
				string attributes = tag.Groups[3].Value;

				if(name == "a") {
					if(closing) {
						page.EndLink();
						linkUrl = null;
					} else {
						Match href = Regex.Match(attributes, "href\\s*=\\s*(\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", RegexOptions.IgnoreCase);
						string target = href.Success ? WebUtility.HtmlDecode(href.Groups[2].Value + href.Groups[3].Value + href.Groups[4].Value) : "";
						if(target.Length > 0 && !target.StartsWith("#") && !target.StartsWith("javascript:", StringComparison.OrdinalIgnoreCase)
							&& Uri.TryCreate(baseUri, target, out Uri? link) && (link.Scheme == "http" || link.Scheme == "https")) {
							linkUrl = link.AbsoluteUri;
						} else {
							linkUrl = null;
						}
					}
				} else if(name == "li" && !closing) {
					page.Break();
					page.AddText("* ", null);
				} else if(name == "td" || name == "th") {
					page.AddText(" ", linkUrl);
				} else if(name == "img" && !closing) {
					Match alt = Regex.Match(attributes, "alt\\s*=\\s*(\"([^\"]*)\"|'([^']*)')", RegexOptions.IgnoreCase);
					string text = alt.Success ? WebUtility.HtmlDecode(alt.Groups[2].Value + alt.Groups[3].Value).Trim() : "";
					if(linkUrl != null && text.Length > 0) {
						page.AddText(text, linkUrl);
					}
				} else if(ParagraphTags.Contains(name)) {
					page.Paragraph();
				} else if(BlockTags.Contains(name)) {
					page.Break();
				}
				if(page.Full) {
					break;
				}
			}
			if(pos < html.Length && !page.Full) {
				page.AddText(WebUtility.HtmlDecode(html.Substring(pos)), linkUrl);
			}
			page.EndLink();
			return page;
		}

		private static Page HomePage()
		{
			Page page = new("http://" + HomeAddress + "/");
			//First line the title, an empty line a new paragraph, "[text](address)" a link
			string[] lines = YuxingServerFile.HomeText().Replace("\r", "").Split('\n');
			page.Title = lines[0];
			foreach(string line in lines.Skip(1)) {
				Match link = Regex.Match(line, @"^\[(.+)\]\((.+)\)$");
				if(link.Success) {
					AddHomeLink(page, link.Groups[1].Value, link.Groups[2].Value);
				} else if(line.Length == 0) {
					page.Paragraph();
				} else {
					page.Break();
					page.AddText(line);
				}
			}
			page.EndLink();
			return page;
		}

		private static void AddHomeLink(Page page, string text, string url)
		{
			page.Break();
			page.AddText(text, url);
			page.EndLink();
		}

		private static Page ErrorPage(string url, string message)
		{
			Page page = new(url);
			page.Title = "无法打开网页";
			page.AddText("无法打开网页：");
			page.Break();
			page.AddText(url);
			page.Paragraph();
			page.AddText(message);
			page.Paragraph();
			page.AddText("按 ESC 返回上一页。");
			return page;
		}

		private static readonly HashSet<char> ClosingPunctuation = new("，。、；：？！）》」』】〕”’．,.;:?!)".ToCharArray());

		internal static bool IsClosing(char c) { return ClosingPunctuation.Contains(c); }

		private static string Clean(string text)
		{
			return Regex.Replace(text, "\\s+", " ").Trim();
		}

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
		private static extern int LCMapStringEx(string lpLocaleName, uint dwMapFlags, string lpSrcStr, int cchSrc, [Out] char[]? lpDestStr, int cchDest, IntPtr lpVersionInformation, IntPtr lpReserved, IntPtr sortHandle);

		//Traditional characters are not in GB2312; Windows can turn them into simplified ones
		private static string ToSimplified(string text)
		{
			if(!OperatingSystem.IsWindows() || text.Length == 0) {
				return text;
			}
			try {
				const uint LCMAP_SIMPLIFIED_CHINESE = 0x02000000;
				char[] output = new char[text.Length * 2 + 16];
				int length = LCMapStringEx("zh-CN", LCMAP_SIMPLIFIED_CHINESE, text, text.Length, output, output.Length, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
				return length > 0 ? new string(output, 0, length) : text;
			} catch {
				return text;
			}
		}

		//One character as the program can show it: ASCII, or a GB2312 pair (symbol rows A1-A9,
		//characters B0-F7); anything else becomes '?'
		internal static byte[] ToGb(char c)
		{
			if(c == '~') {
				return new[] { (byte)'-' };
			}
			if(c < 0x80) {
				return c >= 0x20 ? new[] { (byte)c } : Array.Empty<byte>();
			}
			if(c == '\u00a0' || c == '\u3000') {
				return c == '\u3000' ? new byte[] { 0xA1, 0xA1 } : new[] { (byte)' ' };
			}
			switch(c) {
				case '\u2018': case '\u2019': return new[] { (byte)'\'' };
				case '\u2013': return new[] { (byte)'-' };
				case '\u2022': return new byte[] { 0xA1, 0xA4 };
			}
			byte[] b = _gb!.GetBytes(new[] { c });
			if(b.Length == 2 && ((b[0] >= 0xA1 && b[0] <= 0xA9) || (b[0] >= 0xB0 && b[0] <= 0xF7)) && b[1] >= 0xA1 && b[1] <= 0xFE) {
				return b;
			}
			return new[] { (byte)'?' };
		}

		private class Page
		{
			public string Title = "";
			private readonly string _url;
			private readonly List<List<byte>> _rows = new() { new List<byte>() };
			private int _column = 0;
			private readonly List<(int Row, int Column, string Url)> _links = new();
			private string? _openLink;
			//A space waits until something follows it, so none ends a row or starts one
			private bool _pendingSpace = false;

			public Page(string url) { _url = url; }

			public bool Full => _rows.Count >= MaxRows;

			private List<byte> Row => _rows[^1];

			private void NewRow()
			{
				if(_rows.Count < MaxRows) {
					_rows.Add(new List<byte>());
				}
				_column = 0;
				_pendingSpace = false;
			}

			public void Break()
			{
				if(Row.Count > 0) {
					NewRow();
				}
			}

			//One blank row, never two, and none at the top
			public void Paragraph()
			{
				Break();
				if(_rows.Count > 1 && _rows[^2].Count > 0) {
					NewRow();
				}
			}

			public void EndLink()
			{
				if(_openLink != null) {
					Put(new[] { (byte)']' }, 1);
					_openLink = null;
				}
			}

			private void Put(byte[] bytes, int width)
			{
				if(_column + width > Columns) {
					NewRow();
				}
				Row.AddRange(bytes);
				_column += width;
			}

			public void AddText(string text, string? link = null)
			{
				text = ToSimplified(Regex.Replace(text, "\\s+", " "));
				if(text.Length == 0) {
					return;
				}
				if(link != _openLink) {
					EndLink();
				}

				//Words: a run of ASCII letters is kept whole where it fits; a Chinese character
				//can be wrapped before
				int i = 0;
				while(i < text.Length) {
					char c = text[i];
					if(c == ' ') {
						if(_column > 0) {
							_pendingSpace = true;
						}
						i++;
						continue;
					}
					int end = i;
					if(c < 0x80) {
						while(end < text.Length && text[end] < 0x80 && text[end] != ' ') {
							end++;
						}
					} else {
						end = i + 1;
					}
					List<byte> word = new();
					int width = 0;
					for(int k = i; k < end; k++) {
						byte[] b = ToGb(text[k]);
						word.AddRange(b);
						width += b.Length;
					}
					i = end;
					if(word.Count == 0) {
						continue;
					}

					int space = _pendingSpace ? 1 : 0;
					_pendingSpace = false;
					bool startLink = link != null && _openLink == null;
					int lead = startLink ? 1 : 0;
					if(_column > 0 && _column + space + lead + Math.Min(width, Columns - 1) > Columns) {
						NewRow();
						space = 0;
						if(!startLink && ClosingPunctuation.Contains(c)) {
							CarryLastCharacter();
						}
					}
					if(space > 0) {
						Row.Add((byte)' ');
						_column++;
					}
					if(startLink) {
						_links.Add((_rows.Count - 1, _column, link!));
						_openLink = link;
						Put(new[] { (byte)'[' }, 1);
					}
					//A word wider than a row goes out a character at a time
					for(int k = 0; k < word.Count;) {
						int w = word[k] >= 0xA1 && k + 1 < word.Count ? 2 : 1;
						Put(word.GetRange(k, w).ToArray(), w);
						k += w;
					}
				}
			}

			//No row starts with a closing mark: the Chinese character before it comes down with it,
			//unless a link starts there (its place is already noted)
			private void CarryLastCharacter()
			{
				if(_rows.Count < 2 || Row.Count > 0) {
					return;
				}
				List<byte> previous = _rows[^2];
				int n = previous.Count;
				if(n < 2 || previous[n - 2] < 0xA1 || previous[n - 1] < 0xA1) {
					return;
				}
				//A byte is a column
				int row = _rows.Count - 2;
				if(_links.Any(l => l.Row == row && l.Column >= n - 2)) {
					return;
				}
				Row.Add(previous[n - 2]);
				Row.Add(previous[n - 1]);
				previous.RemoveRange(n - 2, 2);
				_column = 2;
			}

			public byte[] Encode()
			{
				while(_rows.Count > 1 && _rows[^1].Count == 0) {
					_rows.RemoveAt(_rows.Count - 1);
				}
				List<byte> output = new();
				output.Add((byte)'T');
				foreach(char c in ToSimplified(Title)) {
					output.AddRange(ToGb(c));
				}
				output.Add((byte)'\n');
				output.AddRange(Encoding.ASCII.GetBytes("U" + _url.Replace("\n", "") + "\n"));
				foreach(List<byte> row in _rows) {
					output.Add((byte)'P');
					output.AddRange(row);
					output.Add((byte)'\n');
				}
				foreach(var link in _links.Where(l => l.Row < _rows.Count)) {
					output.AddRange(Encoding.ASCII.GetBytes("K" + link.Row + " " + link.Column + " " + new Uri(link.Url).AbsoluteUri + "\n"));
				}
				return output.ToArray();
			}
		}
	}
}
