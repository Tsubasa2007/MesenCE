using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;

namespace Mesen.Utilities
{
	//A video track on a disc the emulated machine has mounted, and how to hand it to whatever
	//the user plays MPEG files with.
	//
	//The machine's own player cannot show these: a VCD track is 352x288 MPEG-1, and the PPU
	//draws 8x8 tiles out of a 64-colour palette - the picture on a real machine comes from a
	//separate decoder chip which is not emulated. Opening the track outside the emulator is
	//not that feature; it is a way to see what is on the disc.
	public class VideoCdTrack
	{
		//Track 1 is the filesystem and is not offered, so numbering starts at 2
		public int Number { get; init; }
		public uint Lba { get; init; }
		public uint Sectors { get; init; }

		//A segment play item rather than a track of its own - see ReadSegmentItems. It is
		//named by the file it comes out of, and none of the track numbering applies to it.
		public string? SegmentName { get; init; }
		public bool IsSegment => SegmentName != null;

		//A segment item whose clock never moves: one of the menu screens the disc's player
		//draws, rather than something with a running time. See ReadMenuStills.
		public bool IsStill { get; init; }

		public TimeSpan Duration => TimeSpan.FromSeconds(Sectors / 75.0);

		//The disc counts its filesystem as track 1, so the first video sits in track 2. Both
		//the files on the disc (AVSEQ01 in track 2) and the machine's own scripts call that
		//one video 1, so naming it by the raw track number is off by one against everything
		//else that names it.
		public int VideoNumber => Number - 1;
		public string Label => IsStill
			? SegmentName ?? ""
			: IsSegment
				? $@"{SegmentName} ({Duration:mm\:ss})"
				: $@"Video {VideoNumber} ({Duration:mm\:ss})";

		private const int RawSectorSize = 2352;
		//Mode 2 form 2: 12 sync + 4 header + 8 subheader, then the payload
		private const int PayloadOffset = 24;
		public const int PayloadSize = 2324;

		//A VCD track opens with a run of empty sectors before the stream itself - 30 by the
		//standard, but it is read rather than assumed, by looking for the first pack header.
		private static readonly byte[] PackHeader = { 0x00, 0x00, 0x01, 0xBA };

		//How far into a stream a track may open and still be taken as opening at its start.
		//The first pack of a properly cut track reads a hundredth of a second, or four tenths
		//on the discs that were ripped rather than rebuilt; anything past this is the tail of
		//the video before it.
		private const double TailThreshold = 1.0;

		//A disc whose video is not in tracks of its own.
		//
		//The YuXing discs are VCD 2.0 hybrids: one MODE2/2352 data track holding an ISO9660
		//filesystem, with /CDI (the CD-i player application), /VCD (the control files),
		///PROGRAMS (the machine's own software, and the bulk of the disc) and /SEGMENT. The
		//video lives in that last one, as "segment play items" - /SEGMENT/ITEM*.DAT.
		//
		//Most of those are stills. The standard allocation for one is 150 sectors and a disc
		//carries a couple of hundred for its menus, so listing them would bury the few items
		//worth watching. They are told apart by the only thing that really separates them - a
		//still has no running time - so an item is offered when the clock has moved by at
		//least MinSegmentSeconds between its first pack and its last.
		private const double MinSegmentSeconds = 1.0;

		public static List<VideoCdTrack> ReadSegmentItems(string binPath)
		{
			return ReadSegments(binPath, false);
		}

		//The other half of the same walk: the items with no running time at all. Those are the
		//menu screens - on a disc whose programs are picked from a menu the decoder draws,
		//they are the whole of the front end, and there is no way to show them here. They are
		//offered to the same external player the videos go to, in the order the disc lists
		//them, which is the order its menus step through.
		public static List<VideoCdTrack> ReadMenuStills(string binPath)
		{
			return ReadSegments(binPath, true);
		}

		private static List<VideoCdTrack> ReadSegments(string binPath, bool wantStills)
		{
			List<VideoCdTrack> items = new();
			try {
				using FileStream src = File.OpenRead(binPath);
				//What the image actually holds. A directory record is only as trustworthy as
				//the disc it came off: one of these carries records naming four-gigabyte items
				//on a six-hundred-megabyte image, and reading a length like that on trust wrote
				//gigabytes of the wrong thing into the temp folder. Nothing that does not fit
				//is offered.
				uint imageSectors = (uint)(src.Length / RawSectorSize);
				foreach((string name, uint lba, uint size) in ReadIsoDirectory(src, "SEGMENT")) {
					uint sectors = (size + 2047) / 2048;
					if(sectors == 0 || lba >= imageSectors || sectors > imageSectors - lba) {
						continue;
					}
					double? first = SegmentClock(src, lba, false);
					double? last = SegmentClock(src, lba + sectors - 1, true);
					//A still is simply anything with no running time, which includes the ones
					//that answer with no clock at all: the allocation for one is 150 sectors
					//and the picture only fills the front of it, so reading the clock at the
					//far end lands in the blank padding and comes back with nothing.
					bool hasRunningTime = first != null && last != null && last.Value - first.Value >= MinSegmentSeconds;
					if(hasRunningTime == wantStills) {
						continue;
					}
					items.Add(new VideoCdTrack() {
						Number = items.Count + 1,
						Lba = lba,
						Sectors = sectors,
						SegmentName = Path.GetFileNameWithoutExtension(name),
						IsStill = wantStills
					});
				}
			} catch(Exception) {
				//A disc that is not an ISO, or one read while it is being swapped out
			}
			return items;
		}

		//The pack clock at one sector, or the nearest one either side of it, so an item's
		//running time can be had without walking all of it
		//How long the item at an address runs for, or 0 if its clock never moves - which is
		//what a still is. Used when the machine names an item directly rather than the disc
		//offering one from its directory.
		public static double SegmentRunningTime(string binPath, uint lba, uint sectors)
		{
			try {
				using FileStream src = File.OpenRead(binPath);
				double? first = SegmentClock(src, lba, false);
				double? last = SegmentClock(src, lba + sectors - 1, true);
				return first.HasValue && last.HasValue ? Math.Max(0, last.Value - first.Value) : 0;
			} catch {
				return 0;
			}
		}

		private static double? SegmentClock(FileStream src, uint lba, bool searchBack)
		{
			byte[] sector = new byte[RawSectorSize];
			for(uint step = 0; step < 16; step++) {
				long at = (long)(searchBack ? lba - step : lba + step) * RawSectorSize;
				if(at < 0 || at + RawSectorSize > src.Length) {
					return null;
				}
				src.Seek(at, SeekOrigin.Begin);
				if(src.Read(sector, 0, RawSectorSize) != RawSectorSize) {
					return null;
				}
				double? clock = PackClock(sector);
				if(clock != null) {
					return clock;
				}
			}
			return null;
		}

		//One directory of an ISO9660 filesystem inside a raw 2352-byte image. A record is:
		//length at 0, extent LBA at 2, data length at 10, flags at 25 (bit 1 = directory),
		//name length at 32, the name at 33.
		private static List<(string Name, uint Lba, uint Size)> ReadIsoDirectory(FileStream src, string wanted)
		{
			List<(string, uint, uint)> found = new();
			byte[]? pvd = ReadUserData(src, 16);
			if(pvd == null || pvd[1] != (byte)'C' || pvd[2] != (byte)'D' || pvd[3] != (byte)'0' || pvd[4] != (byte)'0' || pvd[5] != (byte)'1') {
				return found;
			}

			//The root directory record sits at offset 156; its extent and length are the
			//little-endian halves at +2 and +10.
			uint rootLba = BitConverter.ToUInt32(pvd, 158);
			uint rootSize = BitConverter.ToUInt32(pvd, 166);
			foreach(var entry in ReadRecords(src, rootLba, rootSize)) {
				if(!entry.IsDir || !entry.Name.Equals(wanted, StringComparison.OrdinalIgnoreCase)) {
					continue;
				}
				foreach(var item in ReadRecords(src, entry.Lba, entry.Size)) {
					if(!item.IsDir) {
						found.Add((item.Name, item.Lba, item.Size));
					}
				}
			}
			return found;
		}

		private static List<(string Name, uint Lba, uint Size, bool IsDir)> ReadRecords(FileStream src, uint lba, uint size)
		{
			List<(string, uint, uint, bool)> records = new();
			uint sectors = (size + 2047) / 2048;
			for(uint i = 0; i < sectors; i++) {
				byte[]? data = ReadUserData(src, lba + i);
				if(data == null) {
					break;
				}
				int pos = 0;
				while(pos < data.Length) {
					int len = data[pos];
					if(len < 34 || pos + len > data.Length) {
						//A zero length is the padding to the end of the sector
						break;
					}
					uint extent = BitConverter.ToUInt32(data, pos + 2);
					uint length = BitConverter.ToUInt32(data, pos + 10);
					bool isDir = (data[pos + 25] & 0x02) != 0;
					int nameLen = data[pos + 32];
					//"." and ".." are one byte, 0x00 and 0x01
					if(nameLen == 1 && data[pos + 33] <= 1) {
						pos += len;
						continue;
					}
					char[] name = new char[nameLen];
					for(int c = 0; c < nameLen; c++) {
						name[c] = (char)data[pos + 33 + c];
					}
					records.Add((new string(name).Split(';')[0], extent, length, isDir));
					pos += len;
				}
			}
			return records;
		}

		//The 2048 bytes of user data in one raw sector, Mode 1 or Mode 2 alike - byte 15 is
		//the mode, and Mode 2 carries an 8-byte subheader ahead of the data
		private static byte[]? ReadUserData(FileStream src, uint lba)
		{
			byte[] sector = new byte[RawSectorSize];
			long at = (long)lba * RawSectorSize;
			if(at + RawSectorSize > src.Length) {
				return null;
			}
			src.Seek(at, SeekOrigin.Begin);
			if(src.Read(sector, 0, RawSectorSize) != RawSectorSize) {
				return null;
			}
			byte[] data = new byte[2048];
			Array.Copy(sector, sector[15] == 2 ? 24 : 16, data, 0, 2048);
			return data;
		}

		//The .cue lists every track's INDEX 01. The pregap convention on these discs is the
		//usual 150 sectors, so the data begins there, and a track runs until the next one
		//starts - or, for the last, until the image ends.
		public static List<VideoCdTrack> ReadCueSheet(string cuePath, out string binPath)
		{
			binPath = "";
			List<VideoCdTrack> tracks = new();
			if(string.IsNullOrEmpty(cuePath) || !File.Exists(cuePath)) {
				return tracks;
			}

			List<uint> starts = new();
			foreach(string line in File.ReadAllLines(cuePath)) {
				Match file = Regex.Match(line, @"FILE\s+""([^""]+)""");
				if(file.Success) {
					binPath = Path.Combine(Path.GetDirectoryName(cuePath) ?? "", file.Groups[1].Value);
				}
				Match index = Regex.Match(line, @"INDEX\s+01\s+(\d+):(\d+):(\d+)");
				if(index.Success) {
					uint msf = (uint)((int.Parse(index.Groups[1].Value) * 60 + int.Parse(index.Groups[2].Value)) * 75 + int.Parse(index.Groups[3].Value));
					starts.Add(msf >= 150 ? msf - 150 : 0);
				}
			}

			//The name inside the sheet is written in whatever code page made it, and these
			//discs name their image in GBK while the file on disk is UTF-8 - the same
			//mismatch the core works around. The pair share a stem in every dump seen.
			if(!File.Exists(binPath)) {
				string guess = Path.ChangeExtension(cuePath, ".bin");
				binPath = File.Exists(guess) ? guess : binPath;
			}
			if(!File.Exists(binPath)) {
				return tracks;
			}

			uint imageEnd = (uint)(new FileInfo(binPath).Length / RawSectorSize);
			for(int i = 1; i < starts.Count; i++) {
				uint data = starts[i] + 150;
				uint end = i + 1 < starts.Count ? starts[i + 1] : imageEnd;
				if(end > data) {
					tracks.Add(new VideoCdTrack() { Number = i + 1, Lba = data, Sectors = end - data });
				}
			}

			//A single-track disc has no video tracks at all, and on these machines that does
			//not mean it carries no video.
			if(tracks.Count == 0) {
				tracks = ReadSegmentItems(binPath);
			}
			return tracks;
		}

		//Strip the sector framing and write the stream out where a player can open it. The
		//result is an ordinary MPEG-1 file; nothing here is specific to this machine.
		//
		//firstSector and lastSector bound the part of the track that is wanted, counted from
		//the track's own start the way the machine addresses it - a play names a start and an
		//end position, and a game disc uses that to take a few seconds out of the middle of a
		//track. Passing 0 for lastSector means the whole of the rest of the track.
		public string ExtractToFile(string binPath, string outPath, uint firstSector = 0, uint lastSector = 0)
		{
			using FileStream src = File.OpenRead(binPath);
			using FileStream dst = File.Create(outPath);
			byte[] sector = new byte[RawSectorSize];
			bool started = false;

			uint from = firstSector;
			//A play names where to stop, and that is not the same as where the track stops: a
			//stretch can run past the boundary, and on a disc whose videos do not line up with
			//its tracks it usually does. Counting sectors from the start position rather than
			//stopping at the end of the track is what the machine itself does.
			uint count = lastSector > firstSector ? lastSector - firstSector : (Sectors > from ? Sectors - from : 0);
			//A segment item is a stream of its own, whole and starting at its own beginning,
			//so nothing below has a tail to drop.
			bool mayOpenWithATail = !IsSegment;
			bool dropTail = false;
			//However much of the front turned out to belong to the video before this one. The
			//play asked for a length, so the read runs on by the same amount rather than
			//coming up short by it.
			uint discarded = 0;

			src.Seek((long)(Lba + from) * RawSectorSize, SeekOrigin.Begin);
			for(uint i = 0; i < count + discarded; i++) {
				if(src.Read(sector, 0, RawSectorSize) != RawSectorSize) {
					break;
				}
				//The subheader says what a sector carries: bits 1-3 are video, audio and data,
				//and a sector with none of them is a gap. The drive plays straight through one,
				//counting the time, so this does too. Stopping at the first gap returns
				//whatever fragment came before it, and on the disc here that is the tail of
				//the previous video: its tracks are out of step with their contents by a
				//second more each time, so from the fourth on, a gap-stopped play gave 0.4,
				//1.4, 2.4 seconds and so on of the wrong video instead of the whole of the
				//right one.
				if((sector[18] & 0x0E) == 0) {
					continue;
				}

				double? clock = PackClock(sector);
				if(!started) {
					if(clock == null) {
						continue;
					}
					started = true;

					//A track that opens part way through a stream is opening with the tail of
					//the video before it - the drift on the disc here, which grows by a second
					//a track until the last video is nothing else. Whatever a player makes of
					//the length is then wrong: it reads a clock of eighty seconds at the front
					//and one of seventy at the back, so the position sits at the end from the
					//moment it opens.
					//
					//Only when the play starts where the track does. One that names a position
					//part way in was meant to start there, and its own start is not a tail.
					dropTail = mayOpenWithATail && from == 0 && clock.Value >= TailThreshold;
				} else if(dropTail && clock.HasValue && clock.Value < TailThreshold) {
					//The clock has gone back to the beginning, so everything so far belonged
					//to the video before this one
					discarded = i;
					dst.SetLength(0);
					dst.Position = 0;
					dropTail = false;
				}
				dst.Write(sector, PayloadOffset, PayloadSize);
			}
			return outPath;
		}

		//The clock a pack header carries, in seconds. It is what a player measures a stream's
		//length with, and it is the only thing that says where one video ends and the next
		//begins: 00 00 01 BA, then 33 bits of 90kHz clock spread across five bytes around two
		//marker bits. Null when the sector does not begin a pack.
		private static double? PackClock(byte[] sector)
		{
			for(int b = 0; b < PackHeader.Length; b++) {
				if(sector[PayloadOffset + b] != PackHeader[b]) {
					return null;
				}
			}

			int o = PayloadOffset + PackHeader.Length;
			long high = (sector[o] >> 1) & 0x07;
			long mid = (((sector[o + 1] << 8) | sector[o + 2]) >> 1) & 0x7FFF;
			long low = (((sector[o + 3] << 8) | sector[o + 4]) >> 1) & 0x7FFF;
			return ((high << 30) | (mid << 15) | low) / 90000.0;
		}

		//Where everything extracted from a disc goes. Nothing here outlives the run that
		//made it: what is wanted twice is wanted within one session, and a disc's videos and
		//menu screens together come to hundreds of megabytes, so the folder is emptied the
		//first time a run asks for it rather than left to grow.
		private static bool _scratchSwept = false;
		private static readonly object ScratchLock = new();
		private static string ScratchPath => Path.Combine(Path.GetTempPath(), "Mesen.VideoCd");

		//Emptied once a run, and at startup rather than on the way out: a run that ends in a
		//crash never gets to tidy up after itself, and waiting until someone opens the next
		//video means a folder left by one that did crash can sit there for good. Another copy
		//of the emulator may still be playing one of these, so it goes a file at a time and
		//whatever will not delete is left where it is.
		public static void SweepScratchFolder()
		{
			lock(ScratchLock) {
				if(_scratchSwept) {
					return;
				}
				_scratchSwept = true;
			}

			try {
				if(!Directory.Exists(ScratchPath)) {
					return;
				}
				foreach(string file in Directory.EnumerateFiles(ScratchPath, "*", SearchOption.AllDirectories)) {
					try {
						File.Delete(file);
					} catch(Exception) {
						//Open in a player somewhere - leave it and take the rest
					}
				}
				foreach(string dir in Directory.EnumerateDirectories(ScratchPath)) {
					try {
						Directory.Delete(dir, true);
					} catch(Exception) {
						//Whatever could not be emptied above
					}
				}
			} catch(Exception) {
				//The folder went away underneath us, or is not readable at all
			}
		}

		public static string GetScratchFolder()
		{
			SweepScratchFolder();
			Directory.CreateDirectory(ScratchPath);
			return ScratchPath;
		}

		//Hand the file to whatever the system opens MPEG files with. Deliberately not a
		//player of our own: see the note at the top of this file.
		public static void OpenInPlayer(string path)
		{
			Process.Start(new ProcessStartInfo() { FileName = path, UseShellExecute = true });
		}
	}
}
