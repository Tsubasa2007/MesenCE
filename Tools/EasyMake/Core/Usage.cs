using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace EasyMake.Core;

/// <summary>Which resources, backgrounds and speech phrases a script uses. XDOS holds one pack
/// at a time: a desktop with resource= loads it, any other desktop keeps the pack that was loaded
/// when it was reached. So the script's pages are walked from the first one (with SDOS.RES, the
/// player's default) along every jump - menu items, go, and falling through to the next page -
/// and each page's references are counted against the pack it can run with. A resource that is
/// used goes on to mark what it refers to (a hover effect's animation, an animation's images).
/// The walk is generous: a page reachable by any path counts, with every pack it can have.</summary>
public sealed class Usage
{
	/// <summary>pack (upper case) -> object ids in use</summary>
	public readonly Dictionary<string, HashSet<string>> Objects = new(StringComparer.OrdinalIgnoreCase);
	/// <summary>(pack, entry) -> word number -> the first page that speaks it</summary>
	public readonly Dictionary<(string, string), SortedDictionary<int, string>> Words = new();
	/// <summary>background files shown (upper case)</summary>
	public readonly HashSet<string> Backgrounds = new(StringComparer.OrdinalIgnoreCase);
	/// <summary>packs that can be loaded where answer/easy run (they read it at fixed addresses)</summary>
	public readonly HashSet<string> FixedAddressPacks = new(StringComparer.OrdinalIgnoreCase);
	/// <summary>page -> the sprite pictures it draws, for pages whose menu has a hover effect. When the
	/// pointer leaves a menu item with act=, XDOS hides sprites 0-58 (all but the pointer's), not only
	/// the act's, so those pictures vanish</summary>
	public readonly SortedDictionary<string, SortedSet<string>> ActHidesSprites = new(StringComparer.OrdinalIgnoreCase);

	public bool Uses(string pack, string oid) => Objects.TryGetValue(pack, out var s) && s.Contains(oid);

	public SortedDictionary<int, string> Spoken(string pack, string entry) =>
		Words.TryGetValue((pack.ToUpperInvariant(), entry.ToLowerInvariant()), out var w) ? w : new SortedDictionary<int, string>();

	static readonly Regex TagRe = new(@"^\s*<(/?\w+)([^>]*)>");

	sealed record Tag(string Name, Dictionary<string, string> Kv);

	/// <summary>script: the script text; metas: pack -> objects.json</summary>
	public static Usage Analyze(string script, IDictionary<string, JsonNode> metas)
	{
		var u = new Usage();
		var tags = new List<Tag>();
		foreach(var line in script.Replace("\r\n", "\n").Split('\n')) {
			var m = TagRe.Match(line);
			if(!m.Success) continue;
			var kv = Project.Attrs(m.Groups[2].Value).ToDictionary(x => x.Key.ToLowerInvariant(), x => x.Value.Trim('"'));
			tags.Add(new Tag(m.Groups[1].Value.ToLowerInvariant(), kv));
		}
		//pages: a new one starts at every <desktop>
		var pages = new List<List<Tag>>();
		foreach(var t in tags) {
			if(t.Name == "desktop" || pages.Count == 0) pages.Add(new List<Tag>());
			pages[^1].Add(t);
		}
		var label = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
		for(int i = 0; i < pages.Count; i++)
			foreach(var t in pages[i])
				if(t.Kv.TryGetValue("name", out var n)) label.TryAdd(n, i);
		string PageName(int i) => pages[i].Select(t => t.Kv.GetValueOrDefault("name")).FirstOrDefault(n => n != null) ?? $"#{i + 1}";

		//walk (page, pack) states
		var seen = new HashSet<(int, string)>();
		var todo = new Stack<(int, string)>();
		if(pages.Count > 0) todo.Push((0, "SDOS.RES"));
		var refs = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
		var actPages = new List<(string page, string pack, List<string> srcs)>();
		while(todo.Count > 0) {
			var (pi, packIn) = todo.Pop();
			var page = pages[pi];
			string pack = page[0].Name == "desktop" && page[0].Kv.TryGetValue("resource", out var r) && r.Length > 0 ? r.ToUpperInvariant() : packIn;
			if(!seen.Add((pi, pack))) continue;
			var used = refs.TryGetValue(pack, out var set) ? set : refs[pack] = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			var drawn = new List<string>();
			bool hasAct = false;
			foreach(var t in page) {
				string Attr(string k) => t.Kv.TryGetValue(k, out var v) && v.Length > 0 ? v : null;
				switch(t.Name) {
					case "desktop":
						if(Attr("background") is string bg) u.Backgrounds.Add(bg);
						if(Attr("backsound") is string bs) used.Add(bs);
						break;
					case "img" or "animate": if(Attr("src") is string s) { used.Add(s); drawn.Add(s); } break;
					case "menuitem": if(Attr("act") is string a) { used.Add(a); hasAct = true; } break;
					case "speak":
						if(Attr("src") is string sp) {
							used.Add(sp);
							if(int.TryParse(Attr("word"), out int w)) {
								var key = (pack, sp.ToLowerInvariant());
								if(!u.Words.TryGetValue(key, out var ws)) u.Words[key] = ws = new SortedDictionary<int, string>();
								ws.TryAdd(w, PageName(pi));
							}
						}
						break;
					case "answer" or "easy": u.FixedAddressPacks.Add(pack); break;
				}
				if(Attr("href") is string h && label.TryGetValue(h, out int to)) todo.Push((to, pack));
			}
			if(hasAct && drawn.Count > 0) actPages.Add((PageName(pi), pack, drawn));
			if(pi + 1 < pages.Count) todo.Push((pi + 1, pack));
		}

		//entry names -> object ids, then everything a used object refers to
		foreach(var (pack, meta) in metas) {
			var objs = meta["objects"]?.AsObject();
			if(objs == null) continue;
			var byEntry = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			if(meta["entries"] is JsonArray ea) foreach(var e in ea) byEntry.TryAdd(e[0].GetValue<string>(), e[1].GetValue<string>());
			else foreach(var o in objs) byEntry.TryAdd(o.Key, o.Key);
			var ids = new HashSet<string>(objs.Select(o => o.Key));
			var live = new HashSet<string>();
			var stack = new Stack<string>();
			if(refs.TryGetValue(pack, out var names))
				foreach(var n in names) if(byEntry.TryGetValue(n, out var oid)) stack.Push(oid);
			while(stack.Count > 0) {
				string id = stack.Pop();
				if(!live.Add(id) || objs[id] is not JsonNode node) continue;
				foreach(var s in Strings(node)) if(ids.Contains(s) && !live.Contains(s)) stack.Push(s);
			}
			u.Objects[pack] = live;
			foreach(var (pg, p, srcs) in actPages)
				if(string.Equals(p, pack, StringComparison.OrdinalIgnoreCase))
					foreach(var s in srcs)
						if(byEntry.TryGetValue(s, out var oid) && objs[oid] is JsonNode n && DrawsSprites(n, objs, 0)) {
							if(!u.ActHidesSprites.TryGetValue(pg, out var list)) u.ActHidesSprites[pg] = list = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
							list.Add(s);
						}
		}
		return u;
	}

	/// <summary>Does an img/animate (hand-written or extracted) put sprites on the screen? Glyph,
	/// text and GR pictures are drawn into the background and survive.</summary>
	static bool DrawsSprites(JsonNode n, JsonObject objs, int depth)
	{
		if(depth > 4) return false;
		switch(n["kind"]?.GetValue<string>()) {
			case "img" or "image":
				if(n["glyphs"]?.GetValue<bool>() == true) return false;
				if(n["image"]?["type"] is JsonNode t) return t.GetValue<int>() == 3;
				return n["source"] != null;
			case "animate" or "anim":
				return n["frames"] is JsonArray fs && fs.Any(f => f?[2]?.GetValue<string>() is string img &&
					(img.EndsWith(".png", StringComparison.OrdinalIgnoreCase) || objs[img] is JsonNode o && DrawsSprites(o, objs, depth + 1)));
		}
		return false;
	}

	/// <summary>Analyze a project folder's script and packs</summary>
	public static Usage ForProject(string project)
	{
		var proj = Project.ReadJson(Path.Combine(project, "project.json"));
		var metas = new Dictionary<string, JsonNode>(StringComparer.OrdinalIgnoreCase);
		foreach(var (name, folder) in proj["res"].AsObject()) metas[name] = Project.ReadJson(Path.Combine(project, folder.GetValue<string>(), "objects.json"));
		return Analyze(File.ReadAllText(Path.Combine(project, proj["script"].GetValue<string>())), metas);
	}

	static IEnumerable<string> Strings(JsonNode n)
	{
		switch(n) {
			case JsonObject o: foreach(var kv in o) if(kv.Value != null) foreach(var s in Strings(kv.Value)) yield return s; break;
			case JsonArray a: foreach(var x in a) if(x != null) foreach(var s in Strings(x)) yield return s; break;
			case JsonValue v when v.TryGetValue(out string str): yield return str; break;
		}
	}
}
