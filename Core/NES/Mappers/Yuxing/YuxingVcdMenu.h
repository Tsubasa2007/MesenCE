#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "NES/Mappers/CdImageFile.h"

//The menu a Video CD carries in EXT/PSD_X.VCD, walked here so that a disc holding a library
//of programs can be browsed the way the machine itself browses one.
//
//These discs have no program of their own to draw a menu with - their filesystem holds only
//the player's own directories and a PROGRAMS folder - because the menu is not a program. It
//is the disc's play sequence descriptor: a tree of lists, each naming a still to put on the
//screen and the choices that still is offering. On the machine the drive's own player walks
//it; nothing on this side did, which is why these discs could only be started by picking a
//program out of a list that no real machine has.
//
//What connects an entry to its program is the entry itself. A play list's header has room
//for a playing time and a wait, and these discs keep the program's address there instead:
//the minute, second and frame of where it starts, which is the form the machine's own seek
//command takes. Counting the play lists in the order they appear only looks the same on
//some discs - one game disc stores the pages of its two catalogues grouped by shape rather
//than in order, and counted that way all but its first six entries started the wrong game.
class YuxingVcdMenu
{
private:
	//What a descriptor can be. A play list is a leaf here: the disc means it as "play these
	//items", and picking one is how a program is chosen.
	static constexpr uint8_t PlayList = 0x10;
	static constexpr uint8_t SelectionList = 0x18;
	static constexpr uint8_t ExtendedSelectionList = 0x1A;

	//Offsets inside the descriptors are counted in units of eight bytes, and this one stands
	//for "nowhere" - a choice the disc leaves empty, or a way out a list does not have.
	static constexpr uint16_t NoOffset = 0xFFFF;

	//Where each kind of descriptor keeps what this needs. A selection list's fields sit at
	//fixed places; the choices follow the header, and after them an extended list carries the
	//rectangles the pointer would be hit-tested against - four for the ways out, then one per
	//choice. Nothing here draws a pointer, so they are only measured, to find the descriptor
	//that comes next.
	static constexpr uint32_t SelectionHeader = 20;
	static constexpr uint32_t SelectionAreas = 16;
	static constexpr uint32_t PlayHeader = 14;

	//The two seconds, at 75 sectors to the second, that come before sector 0 in an address
	static constexpr uint32_t LeadIn = 150;

	vector<uint8_t> _psd;

	//Where every play list is, in the order they appear
	vector<uint32_t> _leaves;

	uint32_t _offset = 0;
	uint32_t _choice = 0;
	bool _open = false;
	bool _hasChoices = false;

	//The still the machine should be looking at, once something has changed it
	bool _showPending = false;
	uint32_t _showItem = 0;

	uint16_t Word(uint32_t at)
	{
		if(at + 1 >= _psd.size()) {
			return NoOffset;
		}
		//Big endian, as everything in a play sequence descriptor is
		return (uint16_t)((_psd[at] << 8) | _psd[at + 1]);
	}

	//A descriptor's address, from one of the two-byte fields that hold one
	uint32_t Target(uint32_t at)
	{
		uint16_t value = Word(at);
		if(value == NoOffset) {
			return Nowhere;
		}
		uint32_t target = (uint32_t)value * 8;
		return target < _psd.size() ? target : Nowhere;
	}

	uint8_t TypeAt(uint32_t at) { return at < _psd.size() ? _psd[at] : 0; }
	uint8_t ChoiceCount(uint32_t at) { return TypeAt(at) == PlayList ? 0 : (at + 2 < _psd.size() ? _psd[at + 2] : 0); }

	//Where a descriptor's choices are listed, and where the one after it begins
	uint32_t ChoiceAt(uint32_t at, uint32_t index) { return Target(at + SelectionHeader + index * 2); }

	uint32_t Length(uint32_t at)
	{
		uint8_t type = TypeAt(at);
		if(type == PlayList) {
			return at + 1 < _psd.size() ? PlayHeader + (uint32_t)_psd[at + 1] * 2 : 0;
		}
		uint32_t count = ChoiceCount(at);
		uint32_t length = SelectionHeader + count * 2;
		if(type == ExtendedSelectionList) {
			length += SelectionAreas + count * 4;
		}
		return length;
	}

	//Reading the descriptors through once, to find where the play lists are. Anything that is
	//not one of the three kinds ends the walk - the descriptors are followed by filler.
	void Index()
	{
		_leaves.clear();
		_hasChoices = false;
		uint32_t at = 0;
		while(at < _psd.size()) {
			uint8_t type = TypeAt(at);
			if(type != PlayList && type != SelectionList && type != ExtendedSelectionList) {
				break;
			}
			if(type == PlayList) {
				_leaves.push_back(at);
			} else if(ChoiceCount(at) > 0) {
				_hasChoices = true;
			}
			uint32_t length = Length(at);
			if(length == 0) {
				break;
			}
			at += length;
			//Every descriptor starts on an eight byte boundary
			at += (8 - (at % 8)) % 8;
		}
	}

	//The first choice at or after the one given that leads anywhere
	bool LiveChoice(uint32_t from, uint32_t& found)
	{
		uint32_t count = ChoiceCount(_offset);
		for(uint32_t i = 0; i < count; i++) {
			uint32_t index = (from + i) % count;
			if(ChoiceAt(_offset, index) != Nowhere) {
				found = index;
				return true;
			}
		}
		return false;
	}

	void Show(uint32_t at)
	{
		_offset = at;
		_choice = 0;
		LiveChoice(0, _choice);
		//A selection list says what to show at a fixed place; a play list lists its items
		//after its header, and the first is what the machine would see first.
		uint32_t item = TypeAt(at) == PlayList ? Word(at + PlayHeader) : Word(at + 18);
		if(item != NoOffset && item != 0) {
			_showItem = item;
			_showPending = true;
		}
	}

	//Following one of the ways out the descriptor itself names - a page key or the timeout
	//takes one. A play list is not one of them: that is a program, and a program is reached
	//by choosing it rather than by walking to it.
	bool Turn(uint32_t at)
	{
		if(!_open) {
			return false;
		}
		uint32_t target = Target(at);
		if(target == Nowhere || target == _offset || TypeAt(target) == PlayList) {
			return false;
		}
		Show(target);
		return true;
	}

public:
	static constexpr uint32_t Nowhere = 0xFFFFFFFF;

	//Reads the disc's menu, if it has one worth walking. A disc whose descriptor holds no
	//lists to choose from is not offering a menu - its own program does the choosing, and only
	//asks the drive for pictures - but it still opens on something: the play list a player
	//starts from is its title screen, and that is walked too - see OpensOnTitle.
	bool Open(CdImageFile& image, uint32_t lba, uint32_t length)
	{
		Close();
		_psd = image.ReadRange((uint64_t)lba * 0x800, length);
		if(_psd.empty()) {
			return false;
		}

		Index();
		if(_leaves.empty() || (!_hasChoices && TypeAt(0) != PlayList)) {
			Close();
			return false;
		}

		_open = true;
		Show(0);
		if(_hasChoices) {
			MessageManager::Log("[YuXing] Disc menu: " + std::to_string(_leaves.size()) +
				" entries to choose from");
		} else {
			MessageManager::Log("[YuXing] Disc opens on its title screen");
		}
		return true;
	}

	void Close()
	{
		_psd.clear();
		_leaves.clear();
		_offset = 0;
		_choice = 0;
		_open = false;
		_hasChoices = false;
		_showPending = false;
		_showItem = 0;
	}

	bool IsOpen() { return _open; }

	//Whether the disc starts on a title screen rather than a menu. Every disc carrying one
	//program does: its first play list is a picture held until a key is pressed - "press
	//play to start" - and the one after it is the loading picture, carrying the program's
	//address (see EntrySector). A player shows the first, and on the key shows the second and
	//loads from that address. Picking the program ahead of it is what left nothing at all on
	//the screen for the whole of the load.
	bool OpensOnTitle() { return _open && !_hasChoices; }

	//Back to where a player starts, which is where it goes when a program leaves
	void Restart()
	{
		if(_open) {
			Show(0);
		}
	}
	uint32_t EntryCount() { return (uint32_t)_leaves.size(); }

	//Where on the disc an entry's program starts, as a sector - see the top of this file. The
	//address counts the two seconds every disc begins with, which a sector number does not.
	uint32_t EntrySector(uint32_t entry)
	{
		if(entry >= _leaves.size() || _leaves[entry] + 12 >= _psd.size()) {
			return Nowhere;
		}
		uint32_t at = _leaves[entry];
		uint32_t address = ((uint32_t)_psd[at + 10] * 60 + _psd[at + 11]) * 75 + _psd[at + 12];
		return address >= LeadIn ? address - LeadIn : Nowhere;
	}

	//The picture an entry puts up while its program loads: its play list's first item
	uint32_t EntryItem(uint32_t entry)
	{
		if(entry >= _leaves.size()) {
			return 0;
		}
		uint16_t item = Word(_leaves[entry] + PlayHeader);
		return item == NoOffset ? 0 : item;
	}

	//The still the menu wants on the screen, handed over once
	bool TakeShow(uint32_t& item)
	{
		if(!_showPending) {
			return false;
		}
		_showPending = false;
		item = _showItem;
		return true;
	}

	//Put the still back up - what a machine coming back from somewhere else would see
	void Refresh() { _showPending = _open; }

	//Step through the choices this list is offering, passing over the empty ones
	void Move(int32_t by)
	{
		uint32_t count = ChoiceCount(_offset);
		if(!_open || count == 0) {
			return;
		}
		uint32_t from = (uint32_t)(((int32_t)_choice + by) % (int32_t)count + (int32_t)count) % count;
		LiveChoice(from, _choice);
	}

	//Take a choice by its number, the way the numbers printed on these menus are meant to be
	//used. The first is not always numbered one - a list says what its own numbering starts
	//at - so it is taken from there.
	bool Number(uint32_t number)
	{
		uint32_t count = ChoiceCount(_offset);
		if(!_open || count == 0 || _offset + 3 >= _psd.size()) {
			return false;
		}
		uint32_t base = _psd[_offset + 3];
		if(number < base || number >= base + count) {
			return false;
		}
		uint32_t index = number - base;
		if(ChoiceAt(_offset, index) == Nowhere) {
			return false;
		}
		_choice = index;
		return true;
	}

	//Follow the choice standing at. Answers with the entry chosen, or Nowhere when the choice
	//only led to another list - in which case that list is now the one on screen.
	uint32_t Enter()
	{
		if(!_open) {
			return Nowhere;
		}

		uint32_t count = ChoiceCount(_offset);
		uint32_t target = count > 0 ? ChoiceAt(_offset, _choice) : Nowhere;
		if(target == Nowhere) {
			//A list with nothing to choose is a disc waiting to be started - the opening
			//screen is one, and on the machine it is left by giving up waiting. Where it
			//would have gone by itself is what a key does here.
			//A title screen is a play list, which keeps where it goes next in a different
			//place from a selection list's timeout.
			target = TypeAt(_offset) == PlayList ? Target(_offset + 6) : Target(_offset + 14);
			if(target == Nowhere) {
				return Nowhere;
			}
		}

		if(TypeAt(target) == PlayList) {
			for(size_t i = 0; i < _leaves.size(); i++) {
				if(_leaves[i] == target) {
					return (uint32_t)i;
				}
			}
			return Nowhere;
		}

		Show(target);
		return Nowhere;
	}

	//Whether this list is offering anything to choose at all
	bool HasChoice()
	{
		uint32_t found = 0;
		return _open && LiveChoice(0, found);
	}

	//A list with nothing to choose on it is a screen that shows itself and then moves on by
	//itself - the opening screen of a disc is one, and it names where to go when the waiting
	//is over. Following that is the only way past it: there is nothing on it to pick, and the
	//machine's own key for leaving the player is not one this can see.
	//
	//A list that does have choices is left alone whatever it names, so a menu that would
	//rather sit and wait for a person does exactly that.
	bool TimeOut()
	{
		//A title screen waits for its key: its play list says so, with a wait of for ever
		if(HasChoice() || TypeAt(_offset) == PlayList) {
			return false;
		}
		return Turn(_offset + 14);
	}

	//The page before and the page after, for a list that runs to more than one screenful.
	//Both are the list's own - it names them at offsets 6 and 8 - and neither is an entry on
	//it: a paged menu draws a button for each, but on the hardware they are the player's own
	//keys, not something the list is offering. Nothing walked them, so everything past the
	//first screenful of a paged list could not be reached at all.
	bool NextPage() { return Turn(_offset + 8); }
	bool PrevPage() { return Turn(_offset + 6); }

	//Back to the list this one came from
	void Leave()
	{
		if(!_open) {
			return;
		}
		uint32_t target = Target(_offset + 10);
		if(target != Nowhere && TypeAt(target) != PlayList) {
			Show(target);
		}
	}
};
