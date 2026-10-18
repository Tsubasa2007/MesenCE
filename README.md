# Mesen 学习机版

*A fork of [MesenCE](https://github.com/nesdev-org/MesenCE) that adds the Chinese learning machines of the 1990s (BBK, Subor SB-2000 / SB97, YuXing, Kingwon / Bung Doctor PC Jr., LingTong / Zoson VCD) and the Super A'Can console.*

本项目基于 Mesen 社区版（MesenCE），加入了九十年代国产电脑学习机的模拟：步步高、小霸王 SB-2000 / SB97、裕兴、科王 / 邦谷小博士、灵童 / 中索 VCD，以及 Super A'Can 游戏机。Mesen 原有的功能全部保留：调试器、即时存档、倒带、录像、着色器等等。

## 下载

最新版本在 [Releases 页面](https://github.com/Tsubasa2007/MesenCE/releases)。下载 `Mesen-bbk-日期-win-x64.zip`，解压到一个单独的文件夹，运行 `Mesen.exe` 即可，不需要另外安装 .NET。

BIOS、软盘和光盘镜像都**不**随本程序提供，请使用自己的备份。

## 支持的机器

| 机器 | 需要的文件 | 模拟的硬件 |
|---|---|---|
| 步步高软驱一号（1.0）、步步高 98 型（电子盘） | BIOS（.nes）＋ 软盘镜像（.img） | 软驱、键盘、鼠标、真人语音、打印机 |
| 步步高语音二号 | BIOS（.nes） | 键盘、语音，可接打印机 |
| 小霸王 SB-2000 | BIOS（.nes）＋ 软盘镜像（.img / .ima） | UM6576 显示芯片、软驱、键盘、鼠标、语音 |
| 小霸王 SB97 | V5.0 卡带的 BIOS（.nes）＋ 软盘镜像（.img / .ima） | 软驱（卡带上的 SB DOS）、键盘、语音、打印机 |
| 裕兴电脑学习机（软驱型、VCD 型） | BIOS（.nes）＋ 软盘镜像（.img），VCD 型还可以用光盘镜像（.cue / .bin / .iso） | 软驱、VCD 光驱与光盘菜单、VCD 画面与声音、键盘、内置鼠标或 D 型串口鼠标、打印机 |
| 科王 KW2000 / KW3000、邦谷小博士（Bung Doctor PC Jr.） | BIOS（.nes）＋ 光盘镜像（.cue / .bin），也可以用软盘镜像（.img） | 光驱、软驱、VCD 画面、键盘、鼠标、打印机 |
| 灵童 SMART-128B（含电子盘版）、中索 VCD（G2002） | BIOS（.nes）＋ 软盘镜像（.woz）；中索 VCD 用光盘镜像（.cue / .bin / .iso），光盘上的游戏（.cdv）也可以直接打开 | 软驱、电子盘、光驱与光盘菜单、键盘、鼠标、语音、打印机 |
| 小霸王中英文电脑学习卡 | 卡带（.nes） | 键盘、鼠标、语音 |
| 裕兴电脑学习卡 | 卡带（.nes） | 键盘、鼠标 |
| Super A'Can | 卡带（.bin，或两片芯片合在一起的 .zip） | 整机，另有 68000 与声音处理器的调试器 |

## 运行截图

每种机器先显示前几张，点「更多」展开其余的。

### 步步高：语音二号、软驱一号（1.0）、98 型

<table>
<tr><td align="center"><img src="docs/screenshots/bbk/01-voice2-boot.png" width="192" alt="语音二号：开机画面"><br><sub>语音二号：开机画面</sub></td><td align="center"><img src="docs/screenshots/bbk/02-voice2-menu.png" width="192" alt="语音二号：学习卡菜单"><br><sub>语音二号：学习卡菜单</sub></td><td align="center"><img src="docs/screenshots/bbk/03-v1-self-test.png" width="192" alt="软驱一号：开机自检"><br><sub>软驱一号：开机自检</sub></td><td align="center"><img src="docs/screenshots/bbk/04-v1-demo.png" width="192" alt="步步高 DEMO 演示系统"><br><sub>步步高 DEMO 演示系统</sub></td></tr>
</table>

<details><summary>更多（26 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/bbk/05-v1-002-wps.png" width="192" alt="002 步步高 WPS 文字处理系统 2.1"><br><sub>002 步步高 WPS 文字处理系统 2.1</sub></td><td align="center"><img src="docs/screenshots/bbk/06-v1-003-typing-game.png" width="192" alt="003 打字游戏"><br><sub>003 打字游戏</sub></td><td align="center"><img src="docs/screenshots/bbk/07-v1-004-learn-dos.png" width="192" alt="004 轻轻松松学 DOS"><br><sub>004 轻轻松松学 DOS</sub></td><td align="center"><img src="docs/screenshots/bbk/08-v1-005-english.png" width="192" alt="005 步步高英语（入门篇）"><br><sub>005 步步高英语（入门篇）</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/bbk/09-v1-006-english-words.png" width="192" alt="006 英语单词步步高（初中版）"><br><sub>006 英语单词步步高（初中版）</sub></td><td align="center"><img src="docs/screenshots/bbk/10-v1-007-spreadsheet.png" width="192" alt="007 步步高电子表格"><br><sub>007 步步高电子表格</sub></td><td align="center"><img src="docs/screenshots/bbk/11-v1-009-household.png" width="192" alt="009 步步高家政管理"><br><sub>009 步步高家政管理</sub></td><td align="center"><img src="docs/screenshots/bbk/12-v1-010-word-problems.png" width="192" alt="010 应用题大全"><br><sub>010 应用题大全</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/bbk/13-v1-011-geometry.png" width="192" alt="011 几何大师"><br><sub>011 几何大师</sub></td><td align="center"><img src="docs/screenshots/bbk/14-v1-016-chinese.png" width="192" alt="016 初中语文辅导与训练"><br><sub>016 初中语文辅导与训练</sub></td><td align="center"><img src="docs/screenshots/bbk/15-v1-119-brain-training.png" width="192" alt="119 智力训练 365"><br><sub>119 智力训练 365</sub></td><td align="center"><img src="docs/screenshots/bbk/16-v1-122-paint.png" width="192" alt="122 神奇画笔"><br><sub>122 神奇画笔</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/bbk/17-v1-126-china-records.png" width="192" alt="126 中国之最（一）"><br><sub>126 中国之最（一）</sub></td><td align="center"><img src="docs/screenshots/bbk/18-v1-128-world-records.png" width="192" alt="128 世界之最"><br><sub>128 世界之最</sub></td><td align="center"><img src="docs/screenshots/bbk/19-v1-129-history.png" width="192" alt="129 博古通今（一）"><br><sub>129 博古通今（一）</sub></td><td align="center"><img src="docs/screenshots/bbk/20-v1-132-historical-figures.png" width="192" alt="132 数风流人物"><br><sub>132 数风流人物</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/bbk/21-v1-190-database.png" width="192" alt="190 跟我学数据库"><br><sub>190 跟我学数据库</sub></td><td align="center"><img src="docs/screenshots/bbk/22-v1-193-windows95.png" width="192" alt="193 跟我学 Windows 95"><br><sub>193 跟我学 Windows 95</sub></td><td align="center"><img src="docs/screenshots/bbk/23-v1-196-cognition.png" width="192" alt="196 认知能力训练"><br><sub>196 认知能力训练</sub></td><td align="center"><img src="docs/screenshots/bbk/24-v1-277-school-english.png" width="192" alt="277 走进生活英语系列·学校英语"><br><sub>277 走进生活英语系列·学校英语</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/bbk/25-v1-gt001-games.png" width="192" alt="GT001 游戏合集"><br><sub>GT001 游戏合集</sub></td><td align="center"><img src="docs/screenshots/bbk/26-98-boot.png" width="192" alt="98 型：开机画面"><br><sub>98 型：开机画面</sub></td><td align="center"><img src="docs/screenshots/bbk/27-98-self-test.png" width="192" alt="98 型：开机自检"><br><sub>98 型：开机自检</sub></td><td align="center"><img src="docs/screenshots/bbk/28-98-desktop.png" width="192" alt="98 型：桌面"><br><sub>98 型：桌面</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/bbk/29-98-t002-learn-dos.png" width="192" alt="98 型专用软件 T002 轻轻松松学 DOS 2.0"><br><sub>98 型专用软件 T002 轻轻松松学 DOS 2.0</sub></td><td align="center"><img src="docs/screenshots/bbk/30-98-gt301-games.png" width="192" alt="GT301 壮志凌云 II / 追击十三 II"><br><sub>GT301 壮志凌云 II / 追击十三 II</sub></td></tr>
</table>
</details>

### 小霸王 SB-2000

<table>
<tr><td align="center"><img src="docs/screenshots/sb2000/01-boot.png" width="192" alt="开机画面"><br><sub>开机画面</sub></td><td align="center"><img src="docs/screenshots/sb2000/02-self-test.png" width="192" alt="硬件检测"><br><sub>硬件检测</sub></td><td align="center"><img src="docs/screenshots/sb2000/03-001-demo.png" width="192" alt="001 系统演示 DEMO"><br><sub>001 系统演示 DEMO</sub></td><td align="center"><img src="docs/screenshots/sb2000/04-002-wps.png" width="192" alt="002 WPS 文字处理"><br><sub>002 WPS 文字处理</sub></td></tr>
</table>

<details><summary>更多（16 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/sb2000/05-003-paint.png" width="192" alt="003 神奇画笔"><br><sub>003 神奇画笔</sub></td><td align="center"><img src="docs/screenshots/sb2000/06-005-windows95.png" width="192" alt="005 跟我学 Windows 95 之一：基础篇"><br><sub>005 跟我学 Windows 95 之一：基础篇</sub></td><td align="center"><img src="docs/screenshots/sb2000/07-014-f-basic.png" width="192" alt="014 F-BASIC 程序设计"><br><sub>014 F-BASIC 程序设计</sub></td><td align="center"><img src="docs/screenshots/sb2000/08-015-foxpro.png" width="192" alt="015 跟我学 FoxPro 之一：基础篇"><br><sub>015 跟我学 FoxPro 之一：基础篇</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/sb2000/09-041-chinese-review.png" width="192" alt="041 小学语文毕业总复习（三）"><br><sub>041 小学语文毕业总复习（三）</sub></td><td align="center"><img src="docs/screenshots/sb2000/10-253-pinyin.png" width="192" alt="253 少儿拼音乐园"><br><sub>253 少儿拼音乐园</sub></td><td align="center"><img src="docs/screenshots/sb2000/11-262-poetry.png" width="192" alt="262 诗词鉴赏"><br><sub>262 诗词鉴赏</sub></td><td align="center"><img src="docs/screenshots/sb2000/12-275-idioms.png" width="192" alt="275 成语故事"><br><sub>275 成语故事</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/sb2000/13-305-english-phonetics.png" width="192" alt="305 英语语音入门"><br><sub>305 英语语音入门</sub></td><td align="center"><img src="docs/screenshots/sb2000/14-349-daily-english.png" width="192" alt="349 英语日常用语·初级篇之四"><br><sub>349 英语日常用语·初级篇之四</sub></td><td align="center"><img src="docs/screenshots/sb2000/15-371-english-dictionary.png" width="192" alt="371 少儿英语图解字典"><br><sub>371 少儿英语图解字典</sub></td><td align="center"><img src="docs/screenshots/sb2000/16-382-plants.png" width="192" alt="382 认识植物（1）"><br><sub>382 认识植物（1）</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/sb2000/17-391-household.png" width="192" alt="391 家政管理"><br><sub>391 家政管理</sub></td><td align="center"><img src="docs/screenshots/sb2000/18-399-solitaire.png" width="192" alt="399 空挡接龙"><br><sub>399 空挡接龙</sub></td><td align="center"><img src="docs/screenshots/sb2000/19-401-number-square.png" width="192" alt="401 数字方阵"><br><sub>401 数字方阵</sub></td><td align="center"><img src="docs/screenshots/sb2000/20-g001-games.png" width="192" alt="G001 热血格斗 + 热血排球"><br><sub>G001 热血格斗 + 热血排球</sub></td></tr>
</table>
</details>

### 小霸王 SB97

<table>
<tr><td align="center"><img src="docs/screenshots/sb97/01-a10-chinese.png" width="192" alt="A10 小学语文基础训练一年级"><br><sub>A10 小学语文基础训练一年级</sub></td><td align="center"><img src="docs/screenshots/sb97/02-b751-algebra.png" width="192" alt="B751 初中代数二年级（上）"><br><sub>B751 初中代数二年级（上）</sub></td><td align="center"><img src="docs/screenshots/sb97/03-g71-geography.png" width="192" alt="G71 中国地理"><br><sub>G71 中国地理</sub></td><td align="center"><img src="docs/screenshots/sb97/04-v01-brain-training.png" width="192" alt="V01 智力训练（一）"><br><sub>V01 智力训练（一）</sub></td></tr>
</table>

<details><summary>更多（3 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/sb97/05-x71-computer-exam.png" width="192" alt="X71 计算机等级考试（初级版）基础知识 DOS 教学"><br><sub>X71 计算机等级考试（初级版）基础知识 DOS 教学</sub></td><td align="center"><img src="docs/screenshots/sb97/06-x91-wps.png" width="192" alt="X91 WPS"><br><sub>X91 WPS</sub></td><td align="center"><img src="docs/screenshots/sb97/07-x92-basic.png" width="192" alt="X92 BASIC"><br><sub>X92 BASIC</sub></td></tr>
</table>
</details>

### 裕兴 VCD 型

<table>
<tr><td align="center"><img src="docs/screenshots/yuxing-vcd/01-boot.png" width="192" alt="开机画面"><br><sub>开机画面</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/02-main-menu.png" width="192" alt="主菜单"><br><sub>主菜单</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/03-2b02-software.png" width="192" alt="2B02 WPS 五笔打字集锦"><br><sub>2B02 WPS 五笔打字集锦</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/04-gam01-games.png" width="192" alt="GAM01 游戏系列①"><br><sub>GAM01 游戏系列①</sub></td></tr>
</table>

<details><summary>更多（12 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/yuxing-vcd/05-gam02-games.png" width="192" alt="GAM02 游戏系列②"><br><sub>GAM02 游戏系列②</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/06-gam03-games.png" width="192" alt="GAM03 游戏系列③"><br><sub>GAM03 游戏系列③</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/07-jsj01-pc-assembly.png" width="192" alt="JSJ-01 电脑大师① PC 机组装与使用"><br><sub>JSJ-01 电脑大师① PC 机组装与使用</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/08-jsj02-typing.png" width="192" alt="JSJ-02 电脑大师② 打字、五笔字型"><br><sub>JSJ-02 电脑大师② 打字、五笔字型</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/yuxing-vcd/09-jsj03a-windows98.png" width="192" alt="JSJ-03 电脑大师③ Windows 98（A）"><br><sub>JSJ-03 电脑大师③ Windows 98（A）</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/10-jsj03b-windows98.png" width="192" alt="JSJ-03 电脑大师③ Windows 98（B）"><br><sub>JSJ-03 电脑大师③ Windows 98（B）</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/11-jsj04-wps2000.png" width="192" alt="JSJ-04 电脑大师④ WPS2000"><br><sub>JSJ-04 电脑大师④ WPS2000</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/12-t03-315l-driver.png" width="192" alt="T03 315L 驱动光盘"><br><sub>T03 315L 驱动光盘</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/yuxing-vcd/13-ypd01-primary.png" width="192" alt="YPD-01 小学系列"><br><sub>YPD-01 小学系列</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/14-ypd02-secondary.png" width="192" alt="YPD-02 中学系列"><br><sub>YPD-02 中学系列</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/15-yxf02-english-title.png" width="192" alt="YXF-02 英语启明星"><br><sub>YXF-02 英语启明星</sub></td><td align="center"><img src="docs/screenshots/yuxing-vcd/16-yxf02-english-lesson.png" width="192" alt="YXF-02 英语启明星：课程"><br><sub>YXF-02 英语启明星：课程</sub></td></tr>
</table>
</details>

### 科王 KW3000（VCD）

<table>
<tr><td align="center"><img src="docs/screenshots/kingwon/01-boot.png" width="192" alt="开机画面"><br><sub>开机画面</sub></td><td align="center"><img src="docs/screenshots/kingwon/02-vcd-player.png" width="192" alt="VCD 播放器"><br><sub>VCD 播放器</sub></td><td align="center"><img src="docs/screenshots/kingwon/03-bungcvcd1-games.png" width="192" alt="游戏大全（一）"><br><sub>游戏大全（一）</sub></td><td align="center"><img src="docs/screenshots/kingwon/04-bungcvcd2-games.png" width="192" alt="游戏大全（二）"><br><sub>游戏大全（二）</sub></td></tr>
</table>

<details><summary>更多（9 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/kingwon/05-bungcvcd3-games.png" width="192" alt="游戏大全（三）"><br><sub>游戏大全（三）</sub></td><td align="center"><img src="docs/screenshots/kingwon/06-bungcvcd4-games.png" width="192" alt="游戏大全（四）"><br><sub>游戏大全（四）</sub></td><td align="center"><img src="docs/screenshots/kingwon/07-bungcvcd5-games.png" width="192" alt="游戏大全（五）"><br><sub>游戏大全（五）</sub></td><td align="center"><img src="docs/screenshots/kingwon/08-wins98-desktop.png" width="192" alt="Wins98 视窗教学专辑：桌面"><br><sub>Wins98 视窗教学专辑：桌面</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/kingwon/09-wins98-encyclopedia.png" width="192" alt="Wins98 视窗教学专辑：少儿百科"><br><sub>Wins98 视窗教学专辑：少儿百科</sub></td><td align="center"><img src="docs/screenshots/kingwon/10-why-questions.png" width="192" alt="科王十万个为什么"><br><sub>科王十万个为什么</sub></td><td align="center"><img src="docs/screenshots/kingwon/11-speaking-english.png" width="192" alt="开口就说英语入门"><br><sub>开口就说英语入门</sub></td><td align="center"><img src="docs/screenshots/kingwon/12-electricity.png" width="192" alt="神奇的电和时间"><br><sub>神奇的电和时间</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/kingwon/13-test-papers.png" width="192" alt="科王电脑 VCD 标准化试题集"><br><sub>科王电脑 VCD 标准化试题集</sub></td></tr>
</table>
</details>

### 中索 VCD

<table>
<tr><td align="center"><img src="docs/screenshots/zoson/01-stardvcd01-desktop.png" width="192" alt="STARDVCD01：文字处理、数据库"><br><sub>STARDVCD01：文字处理、数据库</sub></td><td align="center"><img src="docs/screenshots/zoson/02-starcvcd02-desktop.png" width="192" alt="STARCVCD02：打字学校、英语词霸"><br><sub>STARCVCD02：打字学校、英语词霸</sub></td><td align="center"><img src="docs/screenshots/zoson/03-starcvcd03-desktop.png" width="192" alt="STARCVCD03：益智游戏、电子琴、音乐欣赏"><br><sub>STARCVCD03：益智游戏、电子琴、音乐欣赏</sub></td><td align="center"><img src="docs/screenshots/zoson/04-starcvcd04-games.png" width="192" alt="STARCVCD04：游戏菜单"><br><sub>STARCVCD04：游戏菜单</sub></td></tr>
</table>

### 小霸王中英文电脑学习卡

<table>
<tr><td align="center"><img src="docs/screenshots/subor/01-v1.0-title.png" width="192" alt="V1.0 标题"><br><sub>V1.0 标题</sub></td><td align="center"><img src="docs/screenshots/subor/02-v1.0-menu.png" width="192" alt="V1.0 菜单"><br><sub>V1.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/03-v1.1-menu.png" width="192" alt="V1.1 菜单"><br><sub>V1.1 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/04-v3.0-title.png" width="192" alt="V3.0 标题"><br><sub>V3.0 标题</sub></td></tr>
</table>

<details><summary>更多（26 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/subor/05-v3.0-menu.png" width="192" alt="V3.0 菜单"><br><sub>V3.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/06-v4.0-title.png" width="192" alt="V4.0 标题"><br><sub>V4.0 标题</sub></td><td align="center"><img src="docs/screenshots/subor/07-v4.0-menu.png" width="192" alt="V4.0 菜单"><br><sub>V4.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/08-v5.0-title.png" width="192" alt="V5.0 标题"><br><sub>V5.0 标题</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/subor/09-v5.0-menu.png" width="192" alt="V5.0 菜单"><br><sub>V5.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/10-v6.0-title.png" width="192" alt="V6.0 标题"><br><sub>V6.0 标题</sub></td><td align="center"><img src="docs/screenshots/subor/11-v6.0-menu.png" width="192" alt="V6.0 菜单"><br><sub>V6.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/12-v6.0plus-title.png" width="192" alt="V6.0+ 标题"><br><sub>V6.0+ 标题</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/subor/13-v6.0plus-menu.png" width="192" alt="V6.0+ 菜单"><br><sub>V6.0+ 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/14-v7.0-title.png" width="192" alt="V7.0 标题"><br><sub>V7.0 标题</sub></td><td align="center"><img src="docs/screenshots/subor/15-v7.0-menu.png" width="192" alt="V7.0 菜单"><br><sub>V7.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/16-v8.0-title.png" width="192" alt="V8.0 标题"><br><sub>V8.0 标题</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/subor/17-v8.0-menu.png" width="192" alt="V8.0 菜单"><br><sub>V8.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/18-v9.0-title.png" width="192" alt="V9.0 标题"><br><sub>V9.0 标题</sub></td><td align="center"><img src="docs/screenshots/subor/19-v9.0-menu.png" width="192" alt="V9.0 菜单"><br><sub>V9.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/20-v9.1-menu.png" width="192" alt="V9.1 菜单"><br><sub>V9.1 菜单</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/subor/21-v11.0-title.png" width="192" alt="V11.0 标题"><br><sub>V11.0 标题</sub></td><td align="center"><img src="docs/screenshots/subor/22-v11.0-menu.png" width="192" alt="V11.0 菜单"><br><sub>V11.0 菜单</sub></td><td align="center"><img src="docs/screenshots/subor/23-v12.0-boot.png" width="192" alt="V12.0 开机画面"><br><sub>V12.0 开机画面</sub></td><td align="center"><img src="docs/screenshots/subor/24-v12.0-desktop.png" width="192" alt="V12.0 桌面"><br><sub>V12.0 桌面</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/subor/25-v13.0-desktop.png" width="192" alt="V13.0 桌面"><br><sub>V13.0 桌面</sub></td><td align="center"><img src="docs/screenshots/subor/26-v14.0-boot.png" width="192" alt="V14.0 开机画面"><br><sub>V14.0 开机画面</sub></td><td align="center"><img src="docs/screenshots/subor/27-v14.0-desktop.png" width="192" alt="V14.0 桌面"><br><sub>V14.0 桌面</sub></td><td align="center"><img src="docs/screenshots/subor/28-v15.0-boot.png" width="192" alt="V15.0 开机画面（Windows 2002）"><br><sub>V15.0 开机画面（Windows 2002）</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/subor/29-v15.0-desktop.png" width="192" alt="V15.0 桌面"><br><sub>V15.0 桌面</sub></td><td align="center"><img src="docs/screenshots/subor/30-v15.0-v2-boot.png" width="192" alt="V15.0 另一版本开机画面（Windows 2005）"><br><sub>V15.0 另一版本开机画面（Windows 2005）</sub></td></tr>
</table>
</details>

### 裕兴电脑学习机（软驱型）、裕兴电脑学习卡

<table>
<tr><td align="center"><img src="docs/screenshots/yuxing/01-v4.0-basic.png" width="192" alt="V4.0 BASIC"><br><sub>V4.0 BASIC</sub></td><td align="center"><img src="docs/screenshots/yuxing/02-v5.0-title.png" width="192" alt="V5.0 标题"><br><sub>V5.0 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/03-v7.0-title.png" width="192" alt="V7.0 标题"><br><sub>V7.0 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/04-v7.0-menu.png" width="192" alt="V7.0 菜单"><br><sub>V7.0 菜单</sub></td></tr>
</table>

<details><summary>更多（14 张）</summary>

<table>
<tr><td align="center"><img src="docs/screenshots/yuxing/05-v8.0c-title.png" width="192" alt="V8.0-C 标题"><br><sub>V8.0-C 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/06-v8.0c-menu.png" width="192" alt="V8.0-C 菜单"><br><sub>V8.0-C 菜单</sub></td><td align="center"><img src="docs/screenshots/yuxing/07-v8.2d-title.png" width="192" alt="V8.2-D 标题"><br><sub>V8.2-D 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/08-v8.2d-menu.png" width="192" alt="V8.2-D 菜单"><br><sub>V8.2-D 菜单</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/yuxing/09-v8.3d-title.png" width="192" alt="V8.3-D 标题"><br><sub>V8.3-D 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/10-v8.3d-menu.png" width="192" alt="V8.3-D 菜单"><br><sub>V8.3-D 菜单</sub></td><td align="center"><img src="docs/screenshots/yuxing/11-v9.0-98-title.png" width="192" alt="V9.0-98 标题"><br><sub>V9.0-98 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/12-v9.2f-title.png" width="192" alt="V9.2-F 标题"><br><sub>V9.2-F 标题</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/yuxing/13-v9.2f-menu.png" width="192" alt="V9.2-F 菜单"><br><sub>V9.2-F 菜单</sub></td><td align="center"><img src="docs/screenshots/yuxing/14-v10-title.png" width="192" alt="V10 标题"><br><sub>V10 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/15-v10-desktop.png" width="192" alt="V10 桌面"><br><sub>V10 桌面</sub></td><td align="center"><img src="docs/screenshots/yuxing/16-v11-desktop.png" width="192" alt="V11 桌面"><br><sub>V11 桌面</sub></td></tr>
<tr><td align="center"><img src="docs/screenshots/yuxing/17-new1-title.png" width="192" alt="超级学习机 1 号 标题"><br><sub>超级学习机 1 号 标题</sub></td><td align="center"><img src="docs/screenshots/yuxing/18-new1-desktop.png" width="192" alt="超级学习机 1 号 桌面"><br><sub>超级学习机 1 号 桌面</sub></td></tr>
</table>
</details>

## 快速上手

### 1. 放好 BIOS

把学习机的 BIOS（.nes 文件）放到 `Mesen.exe` 所在的文件夹。

### 2. 步步高、小霸王 SB-2000：直接打开软盘镜像

用 **File → Open** 打开软盘镜像（.img / .ima），也可以把镜像拖进窗口。Mesen 会从软盘本身判断它是步步高的盘还是 SB-2000 的盘，再用 `Mesen.exe` 旁边对应的 BIOS 开机。文件夹里同时有步步高 1.0 和 98 型的 BIOS 时，优先用 1.0，因为大部分软件是为 1.0 写的。

另一种方式：把 BIOS 复制一份，改成和镜像相同的文件名放在一起，例如 `游戏.nes` 和 `游戏.img`，然后打开这个 `.nes`，同名的镜像会自动插入。

### 3. 小霸王 SB97、裕兴、科王、邦谷、灵童、中索：先开机，再放盘

打开 BIOS 的 `.nes` 文件。和 BIOS 同名的软盘镜像（`.img`，灵童是 `.woz`）或光盘镜像（`.cue`，或 `.bin` / `.iso`）会自动放进驱动器，也可以开机后再选。

### 4. 换盘

**Game → Select Disk** 会列出文件夹中的镜像，可以搜索、插入（Insert）、弹出（Eject），用 **Change Folder...** 换到存放镜像的文件夹。**Game → Eject Disk** 直接弹出。软盘和光盘都在这里换。

### 5. 键盘和鼠标

- 载入学习机后，Mesen 会自动接上这台机器的键盘和鼠标（**Settings → NES → Input** 里的 *Automatically configure controllers when loading a game*，默认开启）。
- 接上键盘后，Mesen 的快捷键会停用，按键交给学习机。**只有「暂停」（Pause）例外**，但它的默认键 Esc 在接上键盘时只交给学习机，不会暂停。要暂停可以用菜单，或者在 **Settings → Preferences → Shortcut Keys** 里给 Pause 设一个不含 Esc 的键。
- 在画面上单击即可捕获鼠标。按一下 Alt 键（会切到 Mesen 的菜单栏），或者切换到别的窗口（例如 Alt+Tab），就会释放鼠标。

### 6. 光盘里的视频

裕兴和科王光盘上的 VCD 视频直接在机器画面上播放，画面下方有播放条，可以暂停、拖动、跳过。倒带和读取即时存档时，视频会回到机器当时的位置。**Settings → NES → General → Video CD Settings** 可以改用外部播放器播放，或者跳过视频。

### 7. 打印机

学习机打印出来的内容以 PNG 图片保存在 Mesen 的 `Screenshots` 文件夹里。

### 8. 其他设置

在 **Settings → NES → General** 里：

- *BBK Learning Machine Settings*：屏幕右上角的磁盘指示灯；语音二号接打印机。
- *YuXing Learning Machine Settings*：光驱里没有光盘时跳过 VCD 播放画面；改用 D 型串口鼠标。

### 注意

- 程序写盘时，内容会直接写进软盘镜像文件。
- **即时存档会连同整张软盘一起保存。读取存档时，软盘镜像会被改回存档那一刻的内容。** 请先备份重要的镜像。

## 在 Mesen 上新增的主要功能

- **学习机的硬件模拟**
  - 步步高：Inno / Holtek 两颗 ASIC 的内存与显示分页、分屏、行计数中断、LPC-10 真人语音、uPD765 软驱控制器、ESC/P 针式打印机，98 型的 2MB 电子盘。
  - 小霸王 SB-2000：UM6576 显示芯片（块传输 DMA、扩展的图块模式）、软驱、语音。
  - 裕兴：软驱、VCD 光驱与光盘菜单（按菜单项所指的程序启动）、MMC3 兼容芯片与转换的卡带游戏、XT 键盘、串口鼠标、313 行的画面时序。
  - 科王 / 邦谷：兼容 PPU、310 行的画面时序、光驱与光盘载入程序、软驱、光盘游戏的 MMC2 等芯片、键盘与鼠标、打印机。
  - 小霸王 SB97：V5.0 卡带上的 SB DOS、uPD765 软驱控制器、256KB 程序内存、语音、打印机。
  - 灵童 / 中索：Apple II 式软驱（WOZ 镜像）、电子盘、1MB / 2MB 内存、TMS5220 型语音、中索 VCD 的光驱与光盘载入程序、打印机。
- **VCD 视频解码**：光盘上的 MPEG-1 画面和 MP2 声音在机器自己的画面上播放，带播放条，与即时存档和倒带同步。
- **换盘窗口**：可搜索的软盘 / 光盘列表，插入、弹出、更换文件夹。
- **Super A'Can 游戏机**：整机模拟、手柄设置（**Settings → Other systems → Super A'Can**）、68000 调试器与声音处理器调试器。
- **HD 包**：修正了 HD 包制作器会丢失 CHR RAM 图块的问题，以及 HD 包制作器和渲染器会读到错误精灵指针而崩溃的问题。
- **其他**：没有音频设备时（例如关闭了声音的远程桌面）静音启动而不是崩溃；像素较窄的机器在分辨率变化后保持所选的窗口倍数。

## 致谢

感谢 Mesen 的原作者 Sour，以及维护 MesenCE 的社区。

本项目的学习机模拟建立在前辈们多年的研究之上，没有他们就没有这个项目：

- **钳工（fanoble）**：步步高软驱一号、小霸王 SB-2000、普里奇与奇老师声像卡的模拟器（VirtuaNES 修改版，源代码见 <https://gitee.com/fanoble/emulator-bbk>）。本项目的步步高、SB-2000 模拟，以及语音、软驱、打印机，都是从这份代码移植过来的。
- **惊风**：在钳工的模拟器基础上做的 VirtuaNES 修改版 EX 系列，完善了裕兴、小霸王的模拟，加入了步步高 98 型、邦谷小 PC、科王 2000 / 3000、裕兴 VCD、中索 VCD、灵童等机器的支持，并 dump、发布了大量学习卡 ROM。本项目的裕兴、科王 / 邦谷与步步高 98 型的模拟，是从这个版本的代码移植过来的。
- **NewRisingSun**：NintendulatorNRS 模拟器的作者。本项目中 PC 规格软驱控制器（裕兴、科王 / 邦谷使用）的模拟代码出自他手，科王 / 邦谷与步步高 98 型的模拟最早也源自 NintendulatorNRS。
- **STAR**：修改版 FCEUX 中的裕兴 bin 文件支持，以及和钳工一起破解裕兴的语音数据。
- 为钳工的模拟器提供帮助和资料的朋友：**BaNuI**（SB2000 ROM 与电路）、**轻舞**（软驱一号 ROM）、**重庆仔**（奇老师 ROM）、**汤圆**（奇老师原机器、声像磁带录音）、**天の邪鬼**（普里奇 PAL），以及 **VB步步高**、**花生壳**。
- 为学习机和学习卡 ROM 的 dump 与保存出力的 **TPU**、**maxzhou88（周哥）**，以及 **nesbbs** 论坛。
- 以及所有保存、dump、分享这些机器和软件资料的朋友们。

## 编译

见 [COMPILING.md](COMPILING.md)。

## 许可证

Mesen 以 GPL V3 许可证发布，全文见 <http://www.gnu.org/licenses/gpl-3.0.en.html>。

Copyright (C) 2014-2026 Sour, 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
