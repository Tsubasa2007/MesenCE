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

		public TimeSpan Duration => TimeSpan.FromSeconds(Sectors / 75.0);

		//The disc counts its filesystem as track 1, so the first video sits in track 2. Both
		//the files on the disc (AVSEQ01 in track 2) and the machine's own scripts call that
		//one video 1, so naming it by the raw track number is off by one against everything
		//else that names it.
		public int VideoNumber => Number - 1;
		public string Label => $@"Video {VideoNumber} ({Duration:mm\:ss})";

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
					dropTail = from == 0 && clock.Value >= TailThreshold;
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

		//Hand the file to whatever the system opens MPEG files with. Deliberately not a
		//player of our own: see the note at the top of this file.
		public static void OpenInPlayer(string path)
		{
			Process.Start(new ProcessStartInfo() { FileName = path, UseShellExecute = true });
		}
	}
}
