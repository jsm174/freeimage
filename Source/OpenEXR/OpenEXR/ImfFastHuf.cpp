//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) DreamWorks Animation LLC and Contributors of the OpenEXR Project
//

#include "ImfFastHuf.h"
#include <Iex.h>

#include <string.h>
#include <assert.h>
#include <math.h>
#include <vector>

// Static enabling/disabling the fast huffman decode


#if defined(__APPLE__) && defined(__clang__)
//
// Enabled for clang on Apple platforms (tested):
//
#    define OPENEXR_IMF_ENABLE_FAST_HUF_DECODER

#elif defined(__INTEL_COMPILER) || defined(__GNUC__)
//
// Enabled for ICC, GCC:
//       __i386__   -> x86
//       __x86_64__ -> 64-bit x86
//       __e2k__    -> e2k (MCST Elbrus 2000)

#    if defined(__i386__) || defined(__x86_64__) || defined(__e2k__)
#        define OPENEXR_IMF_ENABLE_FAST_HUF_DECODER
#    endif

#elif defined(_MSC_VER)
//
// Enabled for Visual Studio:
//        _M_IX86 -> x86
//        _M_X64  -> 64bit x86

#    if defined(_M_IX86) || defined(_M_X64)
#        define OPENEXR_IMF_ENABLE_FAST_HUF_DECODER
#    endif
#endif

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_ENTER

//
// Adapted from hufUnpackEncTable - 
// We don't need to reconstruct the code book, just the encoded
// lengths for each symbol. From the lengths, we can build the
// base + offset tables. This should be a bit more efficient
// for sparse code books.
// 
//   table     - ptr to the start of the code length data. Will be
//               updated as we decode data
//
//   numBytes  - size of the encoded table (I think)?
//
//   minSymbol - smallest symbol in the code book
//
//   maxSymbol - largest symbol in the code book. 
//
//   rleSymbol - the symbol to trigger RLE in the encoded bitstream
//

FastHufDecoder::FastHufDecoder
    (const char *&table,
     int numBytes,
     int minSymbol,
     int maxSymbol,
     int rleSymbol)
:
    _rleSymbol (rleSymbol),
    _numSymbols (0),
    _minCodeLength (255),
    _maxCodeLength (0),
    _idToSymbol (0)
{
    //
    // List of symbols that we find with non-zero code lengths
    // (listed in the order we find them). Store these in the
    // same format as the code book stores codes + lengths - 
    // low 6 bits are the length, everything above that is
    // the symbol.
    //

    std::vector<uint64_t> symbols;

    //
    // The 'base' table is the minimum code at each code length. base[i]
    // is the smallest code (numerically) of length i.
    //

    uint64_t base[MAX_CODE_LEN + 1];     

    //
    // The 'offset' table is the position (in sorted order) of the first id
    // of a given code length. Array is indexed by code length, like base.
    //

    uint64_t offset[MAX_CODE_LEN + 1];   

    //
    // Count of how many codes at each length there are. Array is 
    // indexed by code length, like base and offset.
    //

    size_t codeCount[MAX_CODE_LEN + 1];    

    for (int i = 0; i <= MAX_CODE_LEN; ++i)
    {
        codeCount[i] = 0;
        base[i]      = 0xffffffffffffffffULL;
        offset[i]    = 0;
    }

    //
    // Count the number of codes, the min/max code lengths, the number of
    // codes with each length, and record symbols with non-zero code
    // length as we find them.
    //

    const char *currByte     = table;
    uint64_t    currBits     = 0;
    int         currBitCount = 0;

    const int SHORT_ZEROCODE_RUN = 59;
    const int LONG_ZEROCODE_RUN  = 63;
    const int SHORTEST_LONG_RUN  = 2 + LONG_ZEROCODE_RUN - SHORT_ZEROCODE_RUN;

    for (uint64_t symbol = static_cast<uint64_t>(minSymbol); symbol <= static_cast<uint64_t>(maxSymbol); symbol++)
    {
        if (currByte - table >= numBytes)
        {
            throw IEX_NAMESPACE::InputExc ("Error decoding Huffman table "
                                           "(Truncated table data).");
        }

        //
        // Next code length - either:
        //       0-58  (literal code length)
        //       59-62 (various lengths runs of 0)
        //       63    (run of n 0's, with n is the next 8 bits)
        //

        uint64_t codeLen = readBits (6, currBits, currBitCount, currByte);

        if (codeLen == (uint64_t) LONG_ZEROCODE_RUN)
        {
            if (currByte - table >= numBytes)
            {
                throw IEX_NAMESPACE::InputExc ("Error decoding Huffman table "
                                               "(Truncated table data).");
            }

            int runLen = readBits (8, currBits, currBitCount, currByte) +
                         SHORTEST_LONG_RUN;

            if (symbol + runLen > static_cast<uint64_t>(maxSymbol + 1))
            {
                throw IEX_NAMESPACE::InputExc ("Error decoding Huffman table "
                                               "(Run beyond end of table).");
            }
            
            symbol += runLen - 1;

        }
        else if (codeLen >= static_cast<uint64_t>(SHORT_ZEROCODE_RUN))
        {
            int runLen = codeLen - SHORT_ZEROCODE_RUN + 2;

            if (symbol + runLen > static_cast<uint64_t>(maxSymbol + 1))
            {
                throw IEX_NAMESPACE::InputExc ("Error decoding Huffman table "
                                               "(Run beyond end of table).");
            }

            symbol += runLen - 1;

        }
        else if (codeLen != 0)
        {
            symbols.push_back ((symbol << 6) | (codeLen & 63));

            if (codeLen < _minCodeLength)
                _minCodeLength = codeLen;

            if (codeLen > _maxCodeLength)
                _maxCodeLength = codeLen;

            codeCount[codeLen]++;
        }
    }

    for (int i = 0; i < MAX_CODE_LEN; ++i)
        _numSymbols += codeCount[i];

    table = currByte;

    //
    // Compute base - once we have the code length counts, there
    //                is a closed form solution for this
    //

    {
        double* countTmp = new double[_maxCodeLength+1];

        for (int l = _minCodeLength; l <= _maxCodeLength; ++l)
        {
            countTmp[l] = (double)codeCount[l] * 
                          (double)(2ll << (_maxCodeLength-l));
        }
    
        for (int l = _minCodeLength; l <= _maxCodeLength; ++l)
        {
            double tmp = 0;

            for (int k =l + 1; k <= _maxCodeLength; ++k)
                tmp += countTmp[k];
            
            tmp /= (double)(2ll << (_maxCodeLength - l));

            base[l] = (uint64_t)ceil (tmp);
        }

        delete [] countTmp;
    }
   
    //
    // Compute offset - these are the positions of the first
    //                  id (not symbol) that has length [i]
    //

    offset[_maxCodeLength] = 0;

    for (int i= _maxCodeLength - 1; i >= _minCodeLength; i--)
        offset[i] = offset[i + 1] + codeCount[i + 1];

    //
    // Allocate and fill the symbol-to-id mapping. Smaller Ids should be
    // mapped to less-frequent symbols (which have longer codes). Use
    // the offset table to tell us where the id's for a given code 
    // length start off.
    //

    _idToSymbol = new int[_numSymbols];

    uint64_t mapping[MAX_CODE_LEN + 1];
    for (int i = 0; i < MAX_CODE_LEN + 1; ++i) 
        mapping[i] = -1;
    for (int i = _minCodeLength; i <= _maxCodeLength; ++i)
        mapping[i] = offset[i];

    for (std::vector<uint64_t>::const_iterator i = symbols.begin(); 
         i != symbols.end();
         ++i)
    {
        int codeLen = *i & 63;
        int symbol  = *i >> 6;

        if (mapping[codeLen] >= static_cast<uint64_t>(_numSymbols))
        {
            delete[] _idToSymbol;
            _idToSymbol = NULL;
            throw IEX_NAMESPACE::InputExc ("Huffman decode error "
                                           "(Invalid symbol in header).");
        }
        _idToSymbol[mapping[codeLen]] = symbol;
        mapping[codeLen]++;
    }

    //
    // exceptions can be thrown whilst building tables. Delete
    // _idToSynmbol before re-throwing to prevent memory leak
    //
    try
    {
      buildTables(base, offset);
    }catch(...)
    {
            delete[] _idToSymbol;
            _idToSymbol = NULL;
            throw;
    }
}


FastHufDecoder::~FastHufDecoder()
{
    delete[] _idToSymbol;
}


//
// Static check if the decoder is enabled.
//
// ATM, I only have access to little endian hardware for testing,
// so I'm not entirely sure that we are reading fom the bit stream
// properly on BE. 
//
// If you happen to have more obscure hardware, check that the
// byte swapping in refill() is happening sensible, add an endian
// check if needed, and fix the preprocessor magic here.
//

#define READ64(c) \
    ((uint64_t)(c)[0] << 56) | ((uint64_t)(c)[1] << 48) | ((uint64_t)(c)[2] << 40) | \
    ((uint64_t)(c)[3] << 32) | ((uint64_t)(c)[4] << 24) | ((uint64_t)(c)[5] << 16) | \
    ((uint64_t)(c)[6] <<  8) | ((uint64_t)(c)[7] ) 

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    ifdef __INTEL_COMPILER // ICC built-in swap for LE hosts
#        if defined(__i386__) || defined(__x86_64__)
#            undef READ64
#            define READ64(c) _bswap64 (*(const uint64_t*) (c))
#        endif

#    else
#        ifdef __has_builtin
#            if __has_builtin(__builtin_bswap64)
#                undef READ64
#                define READ64(c) __builtin_bswap64 (*(const uint64_t*) (c))
#            endif
#        endif
#    endif
#endif


bool
FastHufDecoder::enabled()
{
#    ifdef OPENEXR_IMF_ENABLE_FAST_HUF_DECODER
    return true;
#    else
    return false;
#    endif
}

//
//
// Built the acceleration tables for lookups on the upper bits
// as well as the 'LJ' tables.
//

void
FastHufDecoder::buildTables (uint64_t *base, uint64_t *offset)
{
    //
    // Build the 'left justified' base table, by shifting base left..
    //

    for (int i = 0; i <= MAX_CODE_LEN; ++i)
    {
        if (base[i] != 0xffffffffffffffffULL)
        {
            _ljBase[i] = base[i] << (64 - i);
        }
        else
        {
            //
            // Unused code length - insert dummy values
            //

            _ljBase[i] = 0xffffffffffffffffULL;
        }
    }

    //
    // Combine some terms into a big fat constant, which for
    // lack of a better term we'll call the 'left justified' 
    // offset table (because it serves the same function
    // as 'offset', when using the left justified base table.
    //

    _ljOffset[0] = offset[0] - _ljBase[0];
    for (int i = 1; i <= MAX_CODE_LEN; ++i)
        _ljOffset[i] = offset[i] - (_ljBase[i] >> (64 - i));

    //
    // Build the acceleration tables for the lookups of
    // short codes ( <= TABLE_LOOKUP_BITS long)
    //

    for (uint64_t i = 0; i < 1 << TABLE_LOOKUP_BITS; ++i)
    {
        uint64_t value = i << (64 - TABLE_LOOKUP_BITS);

        _tableSymbol[i]  = 0xffff;
        _tableCodeLen[i] = 0; 

        for (int codeLen = _minCodeLength; codeLen <= _maxCodeLength; ++codeLen)
        {
            if (_ljBase[codeLen] <= value)
            {
                _tableCodeLen[i] = codeLen;

                uint64_t id = _ljOffset[codeLen] + (value >> (64 - codeLen));
                if (id < static_cast<uint64_t>(_numSymbols))
                {
                    _tableSymbol[i] = _idToSymbol[id];
                }
                else
                {
                    throw IEX_NAMESPACE::InputExc ("Huffman decode error "
                                                   "(Overrun).");
                }
                break;
            }
        }
    }

    //
    // Store the smallest value in the table that points to real data.
    // This should be the entry for the largest length that has 
    // valid data (in our case, non-dummy _ljBase)
    //

    int minIdx = TABLE_LOOKUP_BITS;

    while (minIdx > 0 && _ljBase[minIdx] == 0xffffffffffffffffULL)
        minIdx--;

    if (minIdx < 0)
    {
        //
        // Error, no codes with lengths 0-TABLE_LOOKUP_BITS used.
        // Set the min value such that the table is never tested.
        //

        _tableMin = 0xffffffffffffffffULL;
    }
    else
    {
        _tableMin = _ljBase[minIdx];
    }
}


// 
// For decoding, we're holding onto 2 uint64_t's. 
//
// The first (buffer), holds the next bits from the bitstream to be 
// decoded. For certain paths in the decoder, we only need TABLE_LOOKUP_BITS
// valid bits to decode the next symbol. For other paths, we need a full
// 64-bits to decode a symbol. 
//
// When we need to refill 'buffer', we could pull bits straight from 
// the bitstream. But this is very slow and requires lots of book keeping
// (what's the next bit in the next byte?). Instead, we keep another uint64_t
// around that we use to refill from. While this doesn't cut down on the
// book keeping (still need to know how many valid bits), it does cut
// down on some of the bit shifting crazy and byte access. 
//
// The refill uint64_t (bufferBack) gets left-shifted after we've pulled
// off bits. If we run out of bits in the input bit stream, we just
// shift in 0's to bufferBack. 
//
// The refill act takes numBits from the top of bufferBack and sticks
// them in the bottom of buffer. If there aren't enough bits in bufferBack,
// it gets refilled (to 64-bits) from the input bitstream.
//

inline void
FastHufDecoder::refill
    (uint64_t &buffer,
     int numBits,                       // number of bits to refill
     uint64_t &bufferBack,                 // the next 64-bits, to refill from
     int &bufferBackNumBits,            // number of bits left in bufferBack
     const unsigned char *&currByte,    // current byte in the bitstream
     int &currBitsLeft)                 // number of bits left in the bitsream
{
    // 
    // Refill bits into the bottom of buffer, from the top of bufferBack.
    // Always top up buffer to be completely full.
    //

    buffer |= bufferBack >> (64 - numBits);

    if (bufferBackNumBits < numBits)
    {
        numBits -= bufferBackNumBits;

        // 
        // Refill all of bufferBack from the bitstream. Either grab
        // a full 64-bit chunk, or whatever bytes are left. If we
        // don't have 64-bits left, pad with 0's.
        //

        if (currBitsLeft >= 64)
        {
            bufferBack        = READ64 (currByte); 
            bufferBackNumBits = 64;
            currByte         += sizeof (uint64_t);
            currBitsLeft     -= 8 * sizeof (uint64_t);

        }
        else
        {
            bufferBack        = 0;
            bufferBackNumBits = 64; 

            uint64_t shift = 56;
            
            while (currBitsLeft > 0)
            {
                bufferBack |= ((uint64_t)(*currByte)) << shift;

                currByte++;
                shift        -= 8;
                currBitsLeft -= 8;
            }

            //
            // At this point, currBitsLeft might be negative, just because
            // we're subtracting whole bytes. To keep anyone from freaking
            // out, zero the counter.
            //

            if (currBitsLeft < 0)
                currBitsLeft = 0;
        }

        buffer |= bufferBack >> (64 - numBits);
    }


    //
    // We can have cases where the previous shift of bufferBack is << 64 -
    // this is an undefined operation but tends to create just zeroes.
    // so if we won't have any bits left, zero out bufferBack instead of computing the shift
    //

    if (bufferBackNumBits <= numBits)
    {
        bufferBack = 0;
    }else
    {
        bufferBack = bufferBack << numBits;
    }
    bufferBackNumBits -= numBits;


}

//
// Read the next few bits out of a bitstream. Will be given a backing buffer
// (buffer) that may still have data left over from previous reads
// (bufferNumBits).  Bitstream pointer (currByte) will be advanced when needed.
//

inline uint64_t 
FastHufDecoder::readBits
    (int numBits,
     uint64_t &buffer,             // c
     int &bufferNumBits,        // lc
     const char *&currByte)     // in
{
    while (bufferNumBits < numBits)
    {
        buffer = (buffer << 8) | *(unsigned char*)(currByte++);
        bufferNumBits += 8;
    }

    bufferNumBits -= numBits;
    return (buffer >> bufferNumBits) & ((1 << numBits) - 1);
}


//
// Decode using a the 'One-Shift' strategy for decoding, with a 
// small-ish table to accelerate decoding of short codes.
//
// If possible, try looking up codes into the acceleration table.
// This has a few benefits - there's no search involved; We don't
// need an additional lookup to map id to symbol; we don't need
// a full 64-bits (so less refilling). 
//

void
FastHufDecoder::decode
    (const unsigned char *src,
     int numSrcBits,
     unsigned short *dst, 
     int numDstElems)
{
    if (numSrcBits < 128)
        throw IEX_NAMESPACE::InputExc ("Error choosing Huffman decoder implementation "
                                       "(insufficient number of bits).");

    //
    // Current position (byte/bit) in the src data stream
    // (after the first buffer fill)
    //

    const unsigned char *currByte = src + 2 * sizeof (uint64_t);

    numSrcBits -= 8 * 2 * sizeof (uint64_t);

    //
    // 64-bit buffer holding the current bits in the stream
    //

    uint64_t buffer            = READ64 (src); 
    int   bufferNumBits     = 64;

    //
    // 64-bit buffer holding the next bits in the stream
    //

    uint64_t bufferBack        = READ64 ((src + sizeof (uint64_t))); 
    int   bufferBackNumBits = 64;

    int dstIdx = 0;

    while (dstIdx < numDstElems)
    {
        int  codeLen;
        int  symbol;

        //
        // Test if we can be table accelerated. If so, directly
        // lookup the output symbol. Otherwise, we need to fall
        // back to searching for the code.
        //
        // If we're doing table lookups, we don't really need
        // a re-filled buffer, so long as we have TABLE_LOOKUP_BITS
        // left. But for a search, we do need a refilled table.
        //

        if (_tableMin <= buffer)
        {
            int tableIdx = buffer >> (64 - TABLE_LOOKUP_BITS);

            // 
            // For invalid codes, _tableCodeLen[] should return 0. This
            // will cause the decoder to get stuck in the current spot
            // until we run out of elements, then barf that the codestream
            // is bad.  So we don't need to stick a condition like
            //     if (codeLen > _maxCodeLength) in this inner.
            //

            codeLen = _tableCodeLen[tableIdx];
            symbol  = _tableSymbol[tableIdx];
        }
        else
        {
            if (bufferNumBits < 64)
            {
                refill (buffer,
                        64 - bufferNumBits,
                        bufferBack,
                        bufferBackNumBits,
                        currByte,
                        numSrcBits);

                bufferNumBits = 64;
            }

            // 
            // Brute force search: 
            // Find the smallest length where _ljBase[length] <= buffer
            //

            codeLen = TABLE_LOOKUP_BITS + 1;

            while (_ljBase[codeLen] > buffer && codeLen <= _maxCodeLength)
                codeLen++;

            if (codeLen > _maxCodeLength)
            {
                throw IEX_NAMESPACE::InputExc ("Huffman decode error "
                                               "(Decoded an invalid symbol).");
            }

            uint64_t id = _ljOffset[codeLen] + (buffer >> (64 - codeLen));
            if (id < static_cast<uint64_t>(_numSymbols))
            {
                symbol = _idToSymbol[id];
            }
            else
            {
                throw IEX_NAMESPACE::InputExc ("Huffman decode error "
                                               "(Decoded an invalid symbol).");
            }
        }

        //
        // Shift over bit stream, and update the bit count in the buffer
        //

        buffer = buffer << codeLen;
        bufferNumBits -= codeLen;

        //
        // If we received a RLE symbol (_rleSymbol), then we need
        // to read ahead 8 bits to know how many times to repeat
        // the previous symbol. Need to ensure we at least have
        // 8 bits of data in the buffer
        //

        if (symbol == _rleSymbol)
        {
            if (bufferNumBits < 8)
            {
                refill (buffer,
                        64 - bufferNumBits,
                        bufferBack,
                        bufferBackNumBits,
                        currByte,
                        numSrcBits);

                bufferNumBits = 64;
            }

            int rleCount = buffer >> 56;

            if (dstIdx < 1)
            {
                throw IEX_NAMESPACE::InputExc ("Huffman decode error (RLE code "
                                               "with no previous symbol).");
            }

            if (dstIdx + rleCount > numDstElems)
            {
                throw IEX_NAMESPACE::InputExc ("Huffman decode error (Symbol run "
                                               "beyond expected output buffer length).");
            }

            if (rleCount <= 0) 
            {
                throw IEX_NAMESPACE::InputExc("Huffman decode error"
                                              " (Invalid RLE length)");
            }

            for (int i = 0; i < rleCount; ++i)
                dst[dstIdx + i] = dst[dstIdx - 1];

            dstIdx += rleCount;

            buffer = buffer << 8;
            bufferNumBits -= 8;
        }
        else
        {
            dst[dstIdx] = symbol;
            dstIdx++;
        }

        //
        // refill bit stream buffer if we're below the number of 
        // bits needed for a table lookup
        //

        if (bufferNumBits < TABLE_LOOKUP_BITS)
        {
            refill (buffer,
                    64 - bufferNumBits,
                    bufferBack,
                    bufferBackNumBits,
                    currByte,
                    numSrcBits);

            bufferNumBits = 64;
        }
    }

    if (numSrcBits != 0)
    {
        throw IEX_NAMESPACE::InputExc ("Huffman decode error (Compressed data remains "
                                       "after filling expected output buffer).");
    }
}

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_EXIT
