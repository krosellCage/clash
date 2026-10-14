#if !defined(GAME_SOUND_H)
/*
  NOTE(yigit): Sound for the game side - loading WAV files, and writing
  samples into the buffer the platform hands GameGetSoundSamples.  Carried
  over from the OpenGL project's handmade_sound.h, plus a test tone.

  WAV is a RIFF container: a 12-byte header, then a series of chunks.  Each
  chunk is an 8-byte header - a four character code and a size - followed by
  that many bytes of payload.  We only care about two of them:

    "fmt "  how the samples are encoded - channels, rate, bit depth
    "data"  the samples themselves

  Everything else (LIST, fact, cue, whatever the encoder felt like adding) gets
  skipped by walking past it, which is why this is a loop rather than a fixed
  struct read.

  This does NOT decode MP3, Ogg or anything else compressed - those need a real
  decoder.  Convert to 16-bit PCM WAV first.
*/

#include "platform.h"
#include "arena.h"
#include "game_math.h"

// Packs four characters into the u32 that RIFF actually stores.  Little
// endian, so the first character occupies the lowest byte.
#define RIFF_CODE(a, b, c, d) (((u32)(a) << 0) | ((u32)(b) << 8) | \
                               ((u32)(c) << 16) | ((u32)(d) << 24))

enum
{
    WAVE_ChunkID_RIFF = RIFF_CODE('R', 'I', 'F', 'F'),
    WAVE_ChunkID_WAVE = RIFF_CODE('W', 'A', 'V', 'E'),
    WAVE_ChunkID_fmt  = RIFF_CODE('f', 'm', 't', ' '),
    WAVE_ChunkID_data = RIFF_CODE('d', 'a', 't', 'a'),
};

// NOTE(yigit): pack(1) matters.  The compiler would otherwise insert padding
// to align nSamplesPerSec, and these structs are read straight off the bytes
// in the file, which have no padding.
#pragma pack(push, 1)
struct wave_header
{
    u32 RIFFID;
    u32 Size;
    u32 WAVEID;
};

struct wave_chunk_header
{
    u32 ID;
    u32 Size;
};

struct wave_fmt
{
    u16 FormatTag;
    u16 ChannelCount;
    u32 SamplesPerSecond;
    u32 AvgBytesPerSecond;
    u16 BlockAlign;
    u16 BitsPerSample;
    // Anything past here is optional and only present in extended headers.
};
#pragma pack(pop)

#define WAVE_FORMAT_PCM_TAG 1

struct loaded_sound
{
    i16 *Samples;               // interleaved L,R,L,R - always stereo once loaded
    u32 SampleCount;            // frames, NOT individual i16s
    u32 SamplesPerSecond;
};

// Reads a 16-bit PCM WAV and copies its samples into Arena.  Returns a zeroed
// loaded_sound if anything is wrong, so the caller can carry on in silence.
//
// Mono files are widened to stereo on the way in, so playback never has to
// care how many channels the file had.
internal loaded_sound
LoadWAV(thread_context *Thread, game_memory *Memory, memory_arena *Arena,
        const char *FileName)
{
    loaded_sound Result = {};

    debug_read_file_result File = Memory->DEBUGPlatformReadEntireFile(Thread, FileName);
    if(!File.Contents)
    {
        Memory->DEBUGPlatformLog(Thread, "ERROR: Failed to read sound file: ");
        Memory->DEBUGPlatformLog(Thread, FileName);
        Memory->DEBUGPlatformLog(Thread, "\n");
        return(Result);
    }

    u8 *Start = (u8 *)File.Contents;
    u8 *End = Start + File.ContentsSize;

    if(File.ContentsSize < sizeof(wave_header))
    {
        Memory->DEBUGPlatformLog(Thread, "ERROR: Sound file is too small to be a WAV: ");
        Memory->DEBUGPlatformLog(Thread, FileName);
        Memory->DEBUGPlatformLog(Thread, "\n");
        Memory->DEBUGPlatformFreeFileMemory(Thread, File.Contents);
        return(Result);
    }

    wave_header *Header = (wave_header *)Start;
    if((Header->RIFFID != WAVE_ChunkID_RIFF) || (Header->WAVEID != WAVE_ChunkID_WAVE))
    {
        Memory->DEBUGPlatformLog(Thread, "ERROR: Not a RIFF/WAVE file: ");
        Memory->DEBUGPlatformLog(Thread, FileName);
        Memory->DEBUGPlatformLog(Thread, "\n");
        Memory->DEBUGPlatformFreeFileMemory(Thread, File.Contents);
        return(Result);
    }

    // ---------------------------------------------------------------------
    // Walk the chunks.
    // ---------------------------------------------------------------------
    wave_fmt *Format = 0;
    i16 *SourceSamples = 0;
    u32 SourceDataSize = 0;

    u8 *At = Start + sizeof(wave_header);
    while((At + sizeof(wave_chunk_header)) <= End)
    {
        wave_chunk_header *Chunk = (wave_chunk_header *)At;
        u8 *ChunkData = At + sizeof(wave_chunk_header);

        // A truncated file would otherwise walk us off the end.
        u32 ChunkSize = Chunk->Size;
        if((ChunkData + ChunkSize) > End)
        {
            ChunkSize = (u32)(End - ChunkData);
        }

        if(Chunk->ID == WAVE_ChunkID_fmt)
        {
            if(ChunkSize >= sizeof(wave_fmt))
            {
                Format = (wave_fmt *)ChunkData;
            }
        }
        else if(Chunk->ID == WAVE_ChunkID_data)
        {
            SourceSamples = (i16 *)ChunkData;
            SourceDataSize = ChunkSize;
        }

        // NOTE(yigit): Chunks are 2-byte aligned - an odd-sized chunk is
        // followed by a pad byte that is NOT counted in Size.  Miss this and
        // every chunk after an odd one is misread.
        At = ChunkData + ChunkSize + (ChunkSize & 1);
    }

    if(!Format || !SourceSamples || (SourceDataSize == 0))
    {
        Memory->DEBUGPlatformLog(Thread, "ERROR: WAV is missing its fmt or data chunk: ");
        Memory->DEBUGPlatformLog(Thread, FileName);
        Memory->DEBUGPlatformLog(Thread, "\n");
        Memory->DEBUGPlatformFreeFileMemory(Thread, File.Contents);
        return(Result);
    }

    if((Format->FormatTag != WAVE_FORMAT_PCM_TAG) ||
       (Format->BitsPerSample != 16) ||
       ((Format->ChannelCount != 1) && (Format->ChannelCount != 2)))
    {
        // NOTE(yigit): Anything else would need conversion we do not do here -
        // float samples, 8/24/32 bit, or more than two channels.
        Memory->DEBUGPlatformLog(Thread, "ERROR: WAV must be 16-bit PCM, mono or stereo: ");
        Memory->DEBUGPlatformLog(Thread, FileName);
        Memory->DEBUGPlatformLog(Thread, "\n");
        Memory->DEBUGPlatformFreeFileMemory(Thread, File.Contents);
        return(Result);
    }

    // ---------------------------------------------------------------------
    // Copy into the arena, widening mono to stereo as we go.
    // ---------------------------------------------------------------------
    u32 SourceChannels = Format->ChannelCount;
    u32 FrameCount = SourceDataSize / (u32)(sizeof(i16) * SourceChannels);

    i16 *Destination = PushArray(Arena, FrameCount * 2, i16);

    for(u32 FrameIndex = 0;
        FrameIndex < FrameCount;
        ++FrameIndex)
    {
        if(SourceChannels == 2)
        {
            Destination[FrameIndex*2 + 0] = SourceSamples[FrameIndex*2 + 0];
            Destination[FrameIndex*2 + 1] = SourceSamples[FrameIndex*2 + 1];
        }
        else
        {
            // Same sample to both ears.
            i16 Mono = SourceSamples[FrameIndex];
            Destination[FrameIndex*2 + 0] = Mono;
            Destination[FrameIndex*2 + 1] = Mono;
        }
    }

    Result.Samples = Destination;
    Result.SampleCount = FrameCount;
    Result.SamplesPerSecond = Format->SamplesPerSecond;

    // The decoded samples live in the arena now, so the raw file can go.
    Memory->DEBUGPlatformFreeFileMemory(Thread, File.Contents);

    return(Result);
}

// NOTE(yigit): Something has to fill the buffer every frame.  Leave it alone
// and the card replays whatever samples happened to be sitting there, which is
// audible garbage rather than silence.
internal void
WriteSilence(game_sound_output_buffer *SoundBuffer)
{
    i16 *SampleOut = SoundBuffer->Samples;
    for(int SampleIndex = 0;
        SampleIndex < SoundBuffer->SampleCount;
        ++SampleIndex)
    {
        *SampleOut++ = 0;
        *SampleOut++ = 0;
    }
}

// Fills SoundBuffer from Sound, looping back to the start when it runs out.
// PlayCursor is carried by the caller so it survives between frames.
//
// NOTE(yigit): This does no resampling.  If the file's rate does not match the
// output rate it will play at the wrong pitch and speed - convert the file
// instead.
internal void
PlaySoundLooping(loaded_sound *Sound, u32 *PlayCursor,
                 game_sound_output_buffer *SoundBuffer)
{
    if(!Sound->Samples || (Sound->SampleCount == 0))
    {
        WriteSilence(SoundBuffer);
        return;
    }

    i16 *SampleOut = SoundBuffer->Samples;
    u32 Cursor = *PlayCursor;
    for(int SampleIndex = 0;
        SampleIndex < SoundBuffer->SampleCount;
        ++SampleIndex)
    {
        *SampleOut++ = Sound->Samples[Cursor*2 + 0];
        *SampleOut++ = Sound->Samples[Cursor*2 + 1];

        // This wrap is the whole of "looping".
        ++Cursor;
        if(Cursor >= Sound->SampleCount)
        {
            Cursor = 0;
        }
    }

    *PlayCursor = Cursor;
}

/*
  A pure tone - a sine wave at Hz, Volume from 0 (silent) to 1 (as loud as
  16 bits go).  For hearing that sound works at all, with no file needed.

  Phase is carried by the caller so the wave continues exactly where the last
  buffer stopped.  Restarting it at 0 each time would put a jump in the wave
  at every buffer boundary, and a jump in a wave is a click you can hear.
*/
internal void
WriteTestTone(r32 *Phase, game_sound_output_buffer *SoundBuffer,
              r32 Hz, r32 Volume)
{
    // How far along the wave one sample moves: one full turn (2 pi) per
    // cycle, Hz cycles per second, SamplesPerSecond samples per second.
    r32 Step = 2.0f*Pi32*Hz / (r32)SoundBuffer->SamplesPerSecond;

    i16 *SampleOut = SoundBuffer->Samples;
    for(i32 SampleIndex = 0;
        SampleIndex < SoundBuffer->SampleCount;
        ++SampleIndex)
    {
        i16 Value = (i16)(Sin(*Phase) * Volume * 32767.0f);
        *SampleOut++ = Value;       // left
        *SampleOut++ = Value;       // right

        // Kept within one turn, so it never grows big enough to lose float
        // precision - after an hour it would, and the tone would go rough.
        *Phase += Step;
        if(*Phase > 2.0f*Pi32)
        {
            *Phase -= 2.0f*Pi32;
        }
    }
}

#define GAME_SOUND_H
#endif
