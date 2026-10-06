using System.Text.Json.Nodes;

namespace EasyMake.Core;

/// <summary>'new': a small title made from scratch - pictures, a written tune, two pages and a
/// menu (its pictures are embedded in the program).</summary>
public static class Demo
{
	//The first page draws no sprites: its menu has a hover effect, and when the pointer leaves that
	//item XDOS hides sprites 0-58, so a sprite picture there (the robot) would vanish
	public const string Script = "<desktop resource=\"sdos.res\",background=\"sky.gra\",backsound=\"tune\",name=\"main\">\n" +
		"<img left=2,top=7,src=\"sign\">\n" +
		"<static left=8,top=0,caption=\" 欢迎使用 EasyMake \">\n" +
		"<textbox left=2,top=2,right=29,bottom=5>\n" +
		"这张演示盘由重新制作的\n" +
		"EasyMake 工具生成。\n" +
		"请点击下面的按钮。\n" +
		"<static left=22,top=12,caption=\"下一页\">\n" +
		"<static left=22,top=13,caption=\" 退出 \">\n" +
		"<menu>\n" +
		"<menuitem left=168,top=192,right=224,bottom=208,href=\"page2\",act=\"sparkle\">\n" +
		"<menuitem left=168,top=208,right=224,bottom=224,href=\"bye\">\n" +
		"</menu>\n" +
		"<desktop background=\"sky.gra\",name=\"page2\">\n" +
		"<static left=8,top=0,caption=\"   第二页：动画   \">\n" +
		"<textbox left=2,top=2,right=29,bottom=5>\n" +
		"机器人在向你招手。\n" +
		"动画由两张图片组成。\n" +
		"<static left=22,top=12,caption=\"上一页\">\n" +
		"<static left=22,top=13,caption=\" 退出 \">\n" +
		"<animate time=12,src=\"wave\">\n" +
		"<img left=24,top=150,src=\"robot\">\n" +
		"<menu>\n" +
		"<menuitem left=168,top=192,right=224,bottom=208,href=\"main\">\n" +
		"<menuitem left=168,top=208,right=224,bottom=224,href=\"bye\">\n" +
		"</menu>\n" +
		"</desktop name=\"bye\">\n";

	public const string Tune = "; A square wave melody over a triangle bass, looping\n" +
		"header @songs @tracks\n@songs:\nsong 1 0\n@melody:\ninst 31 02 1D 00\nev FD FE\n" +
		"C-4 10\nC-4 10\nG-4 10\nG-4 10\nA-4 10\nA-4 10\nG-4 20\nF-4 10\nF-4 10\nE-4 10\nE-4 10\nD-4 10\nD-4 10\nC-4 20\n" +
		"ev B0 00\nend\n@bass:\ninst 31 00 1C 00\nev FD FE\n" +
		"C-3 20\nE-3 20\nF-3 20\nC-3 20\nG-2 20\nC-3 20\nG-2 20\nC-3 20\nev B0 00\nend\n" +
		"@tracks:\ntrack $54 $00 @melody\ntrack $7E $02 @bass\n";

	static void Unpack(string name, string path)
	{
		using var s = typeof(Demo).Assembly.GetManifestResourceStream("EasyMake.Demo." + name)
			?? throw new FileNotFoundException("embedded " + name);
		using var f = File.Create(path);
		s.CopyTo(f);
	}

	static JsonArray Frame(int x, int y, string img, int xo, int yo) => new(x, y, img, xo, yo, 0, 0);

	public static void NewProject(string folder)
	{
		if(File.Exists(Path.Combine(folder, "project.json"))) throw new IOException(folder + " already holds a project");
		string res = Path.Combine(folder, "res", "sdos");
		Directory.CreateDirectory(res);
		Directory.CreateDirectory(Path.Combine(folder, "backgrounds"));
		File.WriteAllText(Path.Combine(folder, "script.txt"), Script.Replace("\n", "\r\n"), new System.Text.UTF8Encoding(false));
		Unpack("sky.png", Path.Combine(folder, "backgrounds", "sky.png"));
		foreach(var n in new[] { "robot", "robot_up", "sign", "star_small", "star_big" }) Unpack(n + ".png", Path.Combine(res, n + ".png"));
		File.WriteAllText(Path.Combine(res, "tune.mus"), Tune.Replace("\n", Environment.NewLine));
		var wave = new JsonArray();
		//where the still robot is drawn (left=24), ending arm down: the last frame stays on screen
		for(int i = 0; i < 6; i++) wave.Add((JsonNode)Frame(24, 150, i % 2 == 0 ? "robot_up.png" : "robot.png", 0, 0));
		var objects = new JsonObject {
			["robot"] = new JsonObject { ["kind"] = "img", ["source"] = "robot.png", ["first_sprite"] = 0 },
			//BG glyphs must use the colours of the background they are drawn on
			["sign"] = new JsonObject { ["kind"] = "img", ["source"] = "sign.png", ["glyphs"] = true, ["palette_from"] = "sky" },
			//A hover effect: frames are placed relative to the pointer (x - x origin); it plays to
			//the end before XDOS looks at the mouse again, so keep it short
			["sparkle"] = new JsonObject { ["kind"] = "act", ["first_sprite"] = 40, ["frames"] = new JsonArray(Frame(0, 0, "star_small.png", 8, 8), Frame(0, 0, "star_big.png", 8, 8)) },
			//Without count= an animation plays once and the script waits for it
			["wave"] = new JsonObject { ["kind"] = "animate", ["first_sprite"] = 20, ["frames"] = wave },
			["tune"] = new JsonObject { ["kind"] = "music" },
		};
		Project.WriteJson(Path.Combine(res, "objects.json"), new JsonObject { ["objects"] = objects });
		Project.WriteJson(Path.Combine(folder, "project.json"), new JsonObject {
			["script"] = "script.txt", ["res"] = new JsonObject { ["SDOS.RES"] = "res/sdos" },
			["backgrounds"] = new JsonObject { ["SKY.GRA"] = "backgrounds/sky" } });
	}
}
