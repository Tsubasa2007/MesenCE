using System.Text.Json.Nodes;

namespace EasyMake.Ui;

/// <summary>Interface text in Chinese and English</summary>
public static class S
{
	public static int Lang;

	static readonly Dictionary<string, (string zh, string en)> T = new() {
		["title"] = ("EasyMake 学习机超文本制作工具", "EasyMake - BBK hypertext authoring"),
		["file"] = ("文件", "File"), ["make"] = ("制作", "Build"), ["tools"] = ("工具", "Tools"),
		["settings"] = ("设置", "Settings"), ["lang"] = ("语言", "Language"),
		["new"] = ("新建项目…", "New project…"), ["open"] = ("打开项目…", "Open project…"),
		["extract"] = ("从软盘拆出项目…", "Extract a disk…"), ["save"] = ("保存", "Save"),
		["quit"] = ("退出", "Quit"), ["build"] = ("生成软盘…", "Build disk…"),
		["run"] = ("在 MesenCE 中运行", "Run in MesenCE"), ["verify"] = ("校验软盘…", "Verify a disk…"),
		["speechtool"] = ("语音编码试听…", "Encode speech…"), ["paths"] = ("路径设置…", "Paths…"),
		["pictools"] = ("图片浏览…", "Picture browser…"), ["convtool"] = ("图片转换…", "Convert a picture…"),
		["pb_title"] = ("图片浏览：文件夹里各软盘上的图片", "Picture browser: the pictures on the disks in a folder"),
		["pb_folder"] = ("文件夹", "Folder"), ["pb_load"] = ("刷新", "Reload"),
		["pb_subfolders"] = ("包括子文件夹", "Subfolders too"), ["pb_inside"] = ("包括程序里的图片", "Pictures inside programs"),
		["pb_zoom"] = ("缩放", "Zoom"), ["pb_export"] = ("全部导出为 PNG…", "Export all as PNG…"),
		["pb_status"] = ("已读 {0}/{1} 张软盘，找到 {2} 张图片；{3} 张软盘上没有图片", "Read {0}/{1} disks, {2} pictures; {3} disks without pictures"),
		["pb_bad"] = ("无法读取：{0}", "Unreadable: {0}"),
		["pb_nofolder"] = ("请选择一个文件夹。", "Choose a folder."), ["pb_nodisks"] = ("这个文件夹里没有软盘镜像（.img / .ima）。", "No disk images (.img / .ima) in that folder."),
		["pb_exported"] = ("已导出 {0} 张图片到 {1}", "Exported {0} pictures to {1}"),
		["pv_save"] = ("保存为 PNG…", "Save as PNG…"), ["pv_palette"] = ("调色板：", "Palette: "), ["bytes"] = ("字节", "bytes"),
		["cv_title"] = ("图片转换：电脑图片 → 学习机图片", "Convert a PC picture to a BBK picture"),
		["cv_format"] = ("格式", "Format"), ["cv_aspect"] = ("保持比例", "Keep the shape"), ["cv_dither"] = ("抖动（适合照片）", "Dither (for photographs)"),
		["fmt_Grp"] = ("神奇画笔 整幅画 .GRP（224×208）", "Paint program picture .GRP (224 × 208)"),
		["fmt_Pct"] = ("神奇画笔 剪贴图 .PCT（原大小）", "Paint program cut-out .PCT (its own size)"),
		["fmt_Gra"] = ("全屏背景 .GRA（256×240）", "Full-screen background .GRA (256 × 240)"),
		["fmt_GraTiles"] = ("压缩全屏背景 .GRA（瓦片表，256×240）", "Compressed background .GRA (tile map, 256 × 240)"),
		["cv_note"] = ("学习机每块区域只能用 3 种颜色加底色：GRA 每 16×16 一块、4 组调色板；GRP/PCT 每 8×16 一块、3 组调色板。右边是写入后的实际效果。",
			"The machine gives each area 3 colours plus the backdrop: 16 × 16 areas and 4 palettes in a GRA, 8 × 16 areas and 3 palettes in a GRP/PCT. The right side is read back from the bytes written."),
		["cv_before"] = ("原图", "Original"), ["cv_after"] = ("转换后", "Converted"),
		["cv_info"] = ("{0}，{1}×{2}，{3} 字节", "{0}, {1} × {2}, {3} bytes"),
		["cv_save"] = ("保存文件…", "Save file…"), ["cv_disk"] = ("写入软盘镜像…", "Put on a disk image…"),
		["cv_saved"] = ("已保存：", "Saved: "), ["cv_name_q"] = ("软盘上的文件名（8.3 格式）", "File name on the disk (8.3)"),
		["cv_replace"] = ("软盘上已有 {0}，要替换吗？", "The disk already has {0}. Replace it?"),
		["cv_written"] = ("已写入 {0} 到 {1}，软盘剩余 {2} 字节", "Wrote {0} to {1}; {2} bytes free on the disk"),
		["close"] = ("关闭", "Close"),
		["refresh"] = ("刷新预览", "Refresh preview"),
		["script"] = ("脚本", "Script"), ["backgrounds"] = ("背景", "Backgrounds"), ["pack"] = ("资源包", "Pack"),
		["k_img"] = ("精灵图片", "sprite picture"), ["k_glyph"] = ("字符块图片", "glyph picture"),
		["k_animate"] = ("动画", "animation"), ["k_act"] = ("鼠标效果", "hover effect"),
		["k_speak"] = ("语音", "speech"), ["k_music"] = ("音乐", "music"), ["k_raw"] = ("原始数据", "raw data"),
		["k_image"] = ("图像", "image"), ["k_anim"] = ("动画数据", "animation data"),
		["noproject"] = ("请新建或打开一个项目（文件菜单）。", "Create or open a project (File menu)."),
		["add_res"] = ("添加资源…", "Add resource…"), ["del_res"] = ("删除资源", "Delete resource"),
		["add_bg"] = ("添加背景…", "Add background…"), ["del_bg"] = ("删除背景", "Delete background"),
		["replace_pic"] = ("更换图片…", "Replace picture…"), ["open_folder"] = ("打开所在文件夹", "Open folder"),
		["coords"] = ("坐标", "Position"), ["pixel"] = ("像素", "pixel"), ["cell"] = ("字符格", "cell"),
		["drag_hint"] = ("在预览图上拖动可得到菜单项的范围", "Drag over the preview to get a menu item area"),
		["copied"] = ("已复制到剪贴板：", "Copied to the clipboard: "),
		["insert_item"] = ("插入菜单项", "Insert menu item"),
		["name"] = ("名字", "Name"), ["kind"] = ("类型", "Kind"), ["source"] = ("图片", "Picture"),
		["first_sprite"] = ("起始精灵号", "First sprite"), ["own_palette"] = ("单独调色板", "Own palette"),
		["palette_from"] = ("使用背景的颜色", "Colours of background"),
		["add_frame"] = ("添加帧…", "Add frame…"), ["del_frame"] = ("删除帧", "Delete frame"),
		["up"] = ("上移", "Up"), ["down"] = ("下移", "Down"), ["update"] = ("更新", "Update"),
		["play"] = ("播放", "Play"), ["stop"] = ("停止", "Stop"), ["check"] = ("检查", "Check"),
		["listen"] = ("试听", "Listen"), ["add_rec"] = ("添加录音…", "Add recording…"), ["delete"] = ("删除", "Delete"),
		["phrases"] = ("语句（word=0, 1, 2 …）", "Phrases (word=0, 1, 2 …)"),
		["exact_note"] = ("从原盘拆出的精确数据：像素在索引色 PNG 中，颜色和精灵块在 objects.json 中。",
			"Exact data from a disk: pixels in the indexed PNG, colours and cells in objects.json."),
		["built"] = ("软盘已生成：", "Disk written: "), ["working"] = ("处理中…", "Working…"),
		["done"] = ("完成", "Done"), ["error"] = ("出错", "Error"),
		["need_paths"] = ("请先在“设置 → 路径设置”中选择 MesenCE、BIOS 和模板盘。",
			"Set the MesenCE, BIOS and template disk paths first (Settings → Paths)."),
		["mesen"] = ("MesenCE 程序 (Mesen.exe)", "MesenCE (Mesen.exe)"),
		["bios"] = ("BBK 1.0 BIOS (.nes 或 .zip)", "BBK 1.0 BIOS (.nes or .zip)"),
		["template"] = ("模板盘（带 XDOS 的启动盘，如 004 号盘 V2.0）", "Template disk (a boot disk with XDOS, e.g. disk 004 V2.0)"),
		["browse"] = ("浏览…", "Browse…"), ["ok"] = ("确定", "OK"), ["cancel"] = ("取消", "Cancel"),
		["yes"] = ("是", "Yes"), ["no"] = ("否", "No"),
		["res_name_q"] = ("资源名字（英文字母和数字，最多 12 个）", "Resource name (letters and digits, up to 12)"),
		["bg_name_q"] = ("背景文件名（最多 8 个英文字母和数字）", "Background file name (up to 8 letters and digits)"),
		["bad_name"] = ("名字不合适或已存在。", "That name is not usable or already exists."),
		["confirm_del"] = ("确定删除“{0}”吗？（文件不会被删除）", "Delete “{0}”? (Its files are kept.)"),
		["unsaved"] = ("脚本已修改，要保存吗？", "The script has changed. Save it?"),
		["speech_enc"] = ("编码 {0}：{1} 字节，{2:F1} 秒，包络相关 {3:F2}", "Encoded {0}: {1} bytes, {2:F1} s, envelope match {3:F2}"),
		["music_ok"] = ("音乐检查通过：{0} 字节", "Music assembles: {0} bytes"),
		["mus_saved"] = ("音乐已保存", "Music saved"), ["save_mus"] = ("保存音乐", "Save music"),
		["time_hint"] = ("动画速度在脚本里用 time= 设置", "Set the speed with time= in the script"),
		["act_hint"] = ("位置相对鼠标指针：帧位置 = 指针 + x − x原点", "Relative to the pointer: pointer + x − x origin"),
		["glyph_hint"] = ("字符块图片在脚本里用字符格坐标，left 请用偶数", "Glyph pictures use cell positions; use an even left"),
		["new_folder"] = ("选择一个空文件夹作为新项目", "Choose an empty folder for the new project"),
		["pick_folder"] = ("选择项目文件夹", "Choose a project folder"), ["pick_disk"] = ("选择软盘镜像", "Choose a disk image"),
		["not_empty"] = ("这个文件夹不是空的。", "That folder is not empty."),
		["verify_ok"] = ("校验通过", "Verify passed"), ["verify_fail"] = ("校验失败", "Verify failed"),
		["x_origin"] = ("x原点", "x origin"), ["y_origin"] = ("y原点", "y origin"),
		["frames"] = ("帧", "Frames"),
		["unused"] = ("未使用", "unused"),
		["spoken_on"] = ("在“{0}”页说", "spoken on page {0}"),
		["never_spoken"] = ("未使用", "never spoken"),
		["stale_slot"] = ("word={0}：表中这一项指向旧数据，不是语句", "word={0}: this table slot points at stale data, not a phrase"),
		["fixed_note"] = ("注意：脚本使用了 <answer>/<easy>，它们按固定地址读取 {0}，其中标为“未使用”的资源可能被它们用到。",
			"Note: the script uses <answer>/<easy>, which read {0} at fixed addresses; resources there marked unused may be drawn by them."),
		["act_note"] = ("注意：页面 {0} 的菜单有悬停效果（act=）。鼠标离开该菜单项时 XDOS 会隐藏 0-58 号精灵，{1} 会随之消失。请改用字模图片，或把它们放到别的页面。",
			"Note: page {0} has a hover effect (act=). When the pointer leaves that menu item XDOS hides sprites 0-58, so {1} will vanish. Use glyph pictures, or put them on another page."),
	};

	public static string Get(string key) => T.TryGetValue(key, out var v) ? (Lang == 0 ? v.zh : v.en) : key;
	public static string F(string key, params object[] a) => string.Format(Get(key), a);
}

/// <summary>The editor's settings, kept in ~/.easymake_gui.json</summary>
public sealed class Config
{
	static readonly string PathName = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".easymake_gui.json");
	public static string OverridePath;
	public int Lang;
	public string Mesen = "", Bios = "", Template = "", Recent = "", PictureFolder = "";

	static string File => OverridePath ?? PathName;

	public static Config Load()
	{
		var c = new Config();
		//Beside a MesenCE checkout the editor finds the Release build of Mesen by itself
		for(var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent) {
			string guess = System.IO.Path.Combine(d.FullName, "bin", "win-x64", "Release", "Mesen.exe");
			if(System.IO.File.Exists(guess)) { c.Mesen = guess; break; }
		}
		try {
			var j = JsonNode.Parse(System.IO.File.ReadAllText(File));
			c.Lang = j["lang"]?.GetValue<int>() ?? 0;
			c.Mesen = j["mesen"]?.GetValue<string>() is { Length: > 0 } m ? m : c.Mesen;
			c.Bios = j["bios"]?.GetValue<string>() ?? "";
			c.Template = j["template"]?.GetValue<string>() ?? "";
			c.Recent = j["recent"]?.GetValue<string>() ?? "";
			c.PictureFolder = j["picture_folder"]?.GetValue<string>() ?? "";
		} catch(Exception) {
		}
		return c;
	}

	public void Save()
	{
		try {
			var j = new JsonObject { ["lang"] = Lang, ["mesen"] = Mesen, ["bios"] = Bios, ["template"] = Template, ["recent"] = Recent, ["picture_folder"] = PictureFolder };
			System.IO.File.WriteAllText(File, j.ToJsonString(Core.Project.JsonOut));
		} catch(Exception) {
		}
	}
}
