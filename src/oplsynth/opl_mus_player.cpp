#ifdef _WIN32
#include <io.h>
#endif
#include <string.h>
#include <assert.h>
#include <math.h>

#include "opl_mus_player.h"
#include "doomtype.h"
#include "fmopl.h"
#include "w_wad.h"
#include "templates.h"
#include "c_cvars.h"
#include "i_system.h"
#include "stats.h"

#define IMF_RATE				700.0

EXTERN_CVAR (Int, opl_numchips)

OPLmusicBlock::OPLmusicBlock()
{
	scoredata = NULL;
	NextTickIn = 0;
	LastOffset = 0;
	NumChips = MIN(*opl_numchips, 2);
	Looping = false;
	IsStereo = false;
	io = NULL;
	io = new OPLio;
}

OPLmusicBlock::~OPLmusicBlock()
{
	delete io;
}

void OPLmusicBlock::ResetChips ()
{
	ChipAccess.Enter();
	io->OPLdeinit ();
	NumChips = io->OPLinit(MIN(*opl_numchips, 2), IsStereo);
	ChipAccess.Leave();
}

void OPLmusicBlock::Restart()
{
	OPLstopMusic ();
	OPLplayMusic (127);
	MLtime = 0;
	playingcount = 0;
	LastOffset = 0;
}

OPLmusicFile::OPLmusicFile (FILE *file, BYTE *musiccache, int len)
	: ScoreLen (len)
{
	if (io == NULL)
	{
		return;
	}

	scoredata = new BYTE[len];

	if (file)
	{
		if (fread (scoredata, 1, len, file) != (size_t)len)
		{
fail:		delete[] scoredata;
			scoredata = NULL;
			return;
		}
	}
	else
	{
		memcpy(scoredata, &musiccache[0], len);
	}

	if (0 == (NumChips = io->OPLinit(NumChips)))
	{
		goto fail;
	}

	// Check for RDosPlay raw OPL format
	if (((DWORD *)scoredata)[0] == MAKE_ID('R','A','W','A') &&
			 ((DWORD *)scoredata)[1] == MAKE_ID('D','A','T','A'))
	{
		RawPlayer = RDosPlay;
		if (*(WORD *)(scoredata + 8) == 0)
		{ // A clock speed of 0 is bad
			*(WORD *)(scoredata + 8) = 0xFFFF; 
		}
		SamplesPerTick = LittleShort(*(WORD *)(scoredata + 8)) / ADLIB_CLOCK_MUL;
	}
	// Check for DosBox OPL dump
	else if (((DWORD *)scoredata)[0] == MAKE_ID('D','B','R','A') &&
		((DWORD *)scoredata)[1] == MAKE_ID('W','O','P','L'))
	{
		if (((DWORD *)scoredata)[2] == MAKE_ID(0,0,1,0))
		{
			RawPlayer = DosBox1;
			SamplesPerTick = OPL_SAMPLE_RATE / 1000;
			ScoreLen = MIN<int>(len - 24, LittleLong(((DWORD *)scoredata)[4])) + 24;
		}
		else if (((DWORD *)scoredata)[2] == MAKE_ID(2,0,0,0))
		{
			if (scoredata[20] != 0)
			{
				Printf("Unsupported DOSBox Raw OPL format %d\n", scoredata[20]);
				goto fail;
			}
			if (scoredata[21] != 0)
			{
				Printf("Unsupported DOSBox Raw OPL compression %d\n", scoredata[21]);
				goto fail;
			}
			RawPlayer = DosBox2;
			SamplesPerTick = OPL_SAMPLE_RATE / 1000;
			int headersize = 0x1A + scoredata[0x19];
			ScoreLen = MIN<int>(len - headersize, LittleLong(((DWORD *)scoredata)[3]) * 2) + headersize;
		}
		else
		{
			Printf("Unsupported DOSBox Raw OPL version %d.%d\n", LittleShort(((WORD *)scoredata)[4]), LittleShort(((WORD *)scoredata)[5]));
			goto fail;
		}
	}
	// Check for modified IMF format (includes a header)
	else if (((DWORD *)scoredata)[0] == MAKE_ID('A','D','L','I') &&
		     scoredata[4] == 'B' && scoredata[5] == 1)
	{
		int songlen;
		BYTE *max = scoredata + ScoreLen;
		RawPlayer = IMF;
		SamplesPerTick = OPL_SAMPLE_RATE / IMF_RATE;

		score = scoredata + 6;
		// Skip track and game name
		for (int i = 2; i != 0; --i)
		{
			while (score < max && *score++ != '\0') {}
		}
		if (score < max) score++;	// Skip unknown byte
		if (score + 8 > max)
		{ // Not enough room left for song data
			delete[] scoredata;
			scoredata = NULL;
			return;
		}
		songlen = LittleLong(*(DWORD *)score);
		if (songlen != 0 && (songlen +=4) < ScoreLen - (score - scoredata))
		{
			ScoreLen = songlen + int(score - scoredata);
		}
	}
	else
	{
		goto fail;
	}

	Restart ();
}

OPLmusicFile::~OPLmusicFile ()
{
	if (scoredata != NULL)
	{
		io->OPLdeinit ();
		delete[] scoredata;
		scoredata = NULL;
	}
}

bool OPLmusicFile::IsValid () const
{
	return scoredata != NULL;
}

void OPLmusicFile::SetLooping (bool loop)
{
	Looping = loop;
}

void OPLmusicFile::Restart ()
{
	OPLmusicBlock::Restart();
	WhichChip = 0;
	switch (RawPlayer)
	{
	case RDosPlay:
		score = scoredata + 10;
		SamplesPerTick = LittleShort(*(WORD *)(scoredata + 8)) / ADLIB_CLOCK_MUL;
		break;

	case DosBox1:
		score = scoredata + 24;
		SamplesPerTick = OPL_SAMPLE_RATE / 1000;
		break;

	case DosBox2:
		score = scoredata + 0x1A + scoredata[0x19];
		SamplesPerTick = OPL_SAMPLE_RATE / 1000;
		break;

	case IMF:
		score = scoredata + 6;

		// Skip track and game name
		for (int i = 2; i != 0; --i)
		{
			while (*score++ != '\0') {}
		}
		score++;	// Skip unknown byte
		if (*(DWORD *)score != 0)
		{
			score += 4;		// Skip song length
		}
		break;
	}
	io->SetClockRate(SamplesPerTick);
}

bool OPLmusicBlock::ServiceStream (void *buff, int numbytes)
{
	float *samples1 = (float *)buff;
	int numsamples = numbytes / (sizeof(float) << int(IsStereo));
	bool prevEnded = false;
	bool res = true;

	memset(buff, 0, numbytes);

	ChipAccess.Enter();
	while (numsamples > 0)
	{
		double ticky = NextTickIn;
		int tick_in = int(NextTickIn);
		int samplesleft = MIN(numsamples, tick_in);
		size_t i;

		if (samplesleft > 0)
		{
			for (i = 0; i < io->NumChips; ++i)
			{
				io->chips[i]->Update(samples1, samplesleft);
			}
			OffsetSamples(samples1, samplesleft << int(IsStereo));
			assert(NextTickIn == ticky);
			NextTickIn -= samplesleft;
			assert (NextTickIn >= 0);
			numsamples -= samplesleft;
			samples1 += samplesleft << int(IsStereo);
		}
		
		if (NextTickIn < 1)
		{
			int next = PlayTick();
			assert(next >= 0);
			if (next == 0)
			{ // end of song
				if (!Looping || prevEnded)
				{
					if (numsamples > 0)
					{
						for (i = 0; i < io->NumChips; ++i)
						{
							io->chips[i]->Update(samples1, samplesleft);
						}
						OffsetSamples(samples1, numsamples << int(IsStereo));
					}
					res = false;
					break;
				}
				else
				{
					// Avoid infinite loops from songs that do nothing but end
					prevEnded = true;
					Restart ();
				}
			}
			else
			{
				prevEnded = false;
				io->WriteDelay(next);
				NextTickIn += SamplesPerTick * next;
				assert (NextTickIn >= 0);
				MLtime += next;
			}
		}
	}
	ChipAccess.Leave();
	return res;
}

void OPLmusicBlock::OffsetSamples(float *buff, int count)
{
	// Three out of four of the OPL waveforms are non-negative. Depending on
	// timbre selection, this can cause the output waveform to tend toward
	// very large positive values. Heretic's music is particularly bad for
	// this. This function attempts to compensate by offseting the sample
	// data back to around the [-1.0, 1.0] range.

	double max = -1e10, min = 1e10, offset, step;
	int i, ramp, largest_at = 0;

	// Find max and min values for this segment of the waveform.
	for (i = 0; i < count; ++i)
	{
		if (buff[i] > max)
		{
			max = buff[i];
			largest_at = i;
		}
		if (buff[i] < min)
		{
			min = buff[i];
			largest_at = i;
		}
	}
	// Prefer to keep the offset at 0, even if it means a little clipping.
	if (LastOffset == 0 && min >= -1.1 && max <= 1.1)
	{
		offset = 0;
	}
	else
	{
		offset = (max + min) / 2;
		// If the new offset is close to 0, make it 0 to avoid making another
		// full loop through the sample data.
		if (fabs(offset) < 1/256.0)
		{
			offset = 0;
		}
	}
	// Ramp the offset change so there aren't any abrupt clicks in the output.
	// If the ramp is too short, it can sound scratchy. cblood2.mid is
	// particularly unforgiving of short ramps.
	if (count >= 512)
	{
		ramp = 512;
		step = (offset - LastOffset) / 512;
	}
	else
	{
		ramp = MIN(count, MAX(196, largest_at));
		step = (offset - LastOffset) / ramp;
	}
	offset = LastOffset;
	i = 0;
	if (step != 0)
	{
		for (; i < ramp; ++i)
		{
			buff[i] = float(buff[i] - offset);
			offset += step;
		}
	}
	if (offset != 0)
	{
		for (; i < count; ++i)
		{
			buff[i] = float(buff[i] - offset);
		}
	}
	LastOffset = float(offset);
}

int OPLmusicFile::PlayTick ()
{
	BYTE reg, data;
	WORD delay;

	switch (RawPlayer)
	{
	case RDosPlay:
		while (score < scoredata + ScoreLen)
		{
			data = *score++;
			reg = *score++;
			switch (reg)
			{
			case 0:		// Delay
				if (data != 0)
				{
					return data;
				}
				break;

			case 2:		// Speed change or OPL3 switch
				if (data == 0)
				{
					SamplesPerTick = LittleShort(*(WORD *)(score)) / ADLIB_CLOCK_MUL;
					io->SetClockRate(SamplesPerTick);
					score += 2;
				}
				else if (data == 1)
				{
					WhichChip = 0;
				}
				else if (data == 2)
				{
					WhichChip = 1;
				}
				break;

			case 0xFF:	// End of song
				if (data == 0xFF)
				{
					return 0;
				}
				break;

			default:	// It's something to stuff into the OPL chip
				if (WhichChip < NumChips)
				{
					io->OPLwriteReg(WhichChip, reg, data);
				}
				break;
			}
		}
		break;

	case DosBox1:
		while (score < scoredata + ScoreLen)
		{
			reg = *score++;

			if (reg == 4)
			{
				reg = *score++;
				data = *score++;
			}
			else if (reg == 0)
			{ // One-byte delay
				return *score++ + 1;
			}
			else if (reg == 1)
			{ // Two-byte delay
				int delay = score[0] + (score[1] << 8) + 1;
				score += 2;
				return delay;
			}
			else if (reg == 2)
			{ // Select OPL chip 0
				WhichChip = 0;
				continue;
			}
			else if (reg == 3)
			{ // Select OPL chip 1
				WhichChip = 1;
				continue;
			}
			else
			{
				data = *score++;
			}
			if (WhichChip < NumChips)
			{
				io->OPLwriteReg(WhichChip, reg, data);
			}
		}
		break;

	case DosBox2:
		{
			BYTE *to_reg = scoredata + 0x1A;
			BYTE to_reg_size = scoredata[0x19];
			BYTE short_delay_code = scoredata[0x17];
			BYTE long_delay_code = scoredata[0x18];

			while (score < scoredata + ScoreLen)
			{
				BYTE code = *score++;
				data = *score++;

				// Which OPL chip to write to is encoded in the high bit of the code value.
				int which = !!(code & 0x80);
				code &= 0x7F;

				if (code == short_delay_code)
				{
					return data + 1;
				}
				else if (code == long_delay_code)
				{
					return (data + 1) << 8;
				}
				else if (code < to_reg_size && which < NumChips)
				{
					io->OPLwriteReg(which, to_reg[code], data);
				}
			}
		}
		break;

	case IMF:
		delay = 0;
		while (delay == 0 && score + 4 - scoredata <= ScoreLen)
		{
			if (*(DWORD *)score == 0xFFFFFFFF)
			{ // This is a special value that means to end the song.
				return 0;
			}
			reg = score[0];
			data = score[1];
			delay = LittleShort(((WORD *)score)[1]);
			score += 4;
			io->OPLwriteReg (0, reg, data);
		}
		return delay;
	}
	return 0;
}

/*
ADD_STAT (opl)
{
	return YM3812GetVoiceString ();
}
*/

OPLmusicFile::OPLmusicFile(const OPLmusicFile *source, const char *filename)
{
	ScoreLen = source->ScoreLen;
	scoredata = new BYTE[ScoreLen];
	memcpy(scoredata, source->scoredata, ScoreLen);
	SamplesPerTick = source->SamplesPerTick;
	RawPlayer = source->RawPlayer;
	score = source->score;
	NumChips = source->NumChips;
	WhichChip = 0;
	if (io != NULL)
	{
		delete io;
	}
	io = new DiskWriterIO(filename);
	NumChips = io->OPLinit(NumChips);
	Restart();
}

void OPLmusicFile::Dump()
{
	int time;

	time = PlayTick();
	while (time != 0)
	{
		io->WriteDelay(time);
		time = PlayTick();
	}
}
