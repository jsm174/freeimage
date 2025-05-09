//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Pixar Animation Studios and Contributors of the OpenEXR Project
//

//-----------------------------------------------------------------------------
//
//	class Pxr24Compressor
//
//	This compressor is based on source code that was contributed to
//	OpenEXR by Pixar Animation Studios.  The compression method was
//	developed by Loren Carpenter.
//
//	The compressor preprocesses the pixel data to reduce entropy,
//	and then calls zlib.
//
//	Compression of HALF and UINT channels is lossless, but compressing
//	FLOAT channels is lossy: 32-bit floating-point numbers are converted
//	to 24 bits by rounding the significand to 15 bits.
//
//	When the compressor is invoked, the caller has already arranged
//	the pixel data so that the values for each channel appear in a
//	contiguous block of memory.  The compressor converts the pixel
//	values to unsigned integers: For UINT, this is a no-op.  HALF
//	values are simply re-interpreted as 16-bit integers.  FLOAT
//	values are converted to 24 bits, and the resulting bit patterns
//	are interpreted as integers.  The compressor then replaces each
//	value with the difference between the value and its left neighbor.
//	This turns flat fields in the image into zeroes, and ramps into
//	strings of similar values.  Next, each difference is split into
//	2, 3 or 4 bytes, and the bytes are transposed so that all the
//	most significant bytes end up in a contiguous block, followed
//	by the second most significant bytes, and so on.  The resulting
//	string of bytes is compressed with zlib.
//
//-----------------------------------------------------------------------------

#include "ImfPxr24Compressor.h"
#include "ImfHeader.h"
#include "ImfChannelList.h"
#include "ImfMisc.h"
#include "ImfCheckedArithmetic.h"
#include "ImfNamespace.h"

#include <ImathFun.h>
#include <Iex.h>

#include <half.h>
#include <zlib.h>
#include <assert.h>
#include <algorithm>

using namespace std;
using namespace IMATH_NAMESPACE;

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_ENTER

namespace {

//
// Conversion from 32-bit to 24-bit floating-point numbers.
// Conversion back to 32 bits is simply an 8-bit shift to the left.
//

inline unsigned int
floatToFloat24 (float f)
{
    union
    {
	float		f;
	unsigned int	i;
    } u;

    u.f = f;

    //
    // Disassemble the 32-bit floating point number, f,
    // into sign, s, exponent, e, and significand, m.
    //

    unsigned int s = u.i & 0x80000000;
    unsigned int e = u.i & 0x7f800000;
    unsigned int m = u.i & 0x007fffff;
    unsigned int i;

    if (e == 0x7f800000)
    {
	if (m)
	{
	    //
	    // F is a NAN; we preserve the sign bit and
	    // the 15 leftmost bits of the significand,
	    // with one exception: If the 15 leftmost
	    // bits are all zero, the NAN would turn
	    // into an infinity, so we have to set at
	    // least one bit in the significand.
	    //

	    m >>= 8;
	    i = (e >> 8) | m | (m == 0);
	}
	else
	{
	    //
	    // F is an infinity.
	    //

	    i = e >> 8;
	}
    }
    else
    {
	//
	// F is finite, round the significand to 15 bits.
	//

	i = ((e | m) + (m & 0x00000080)) >> 8;

	if (i >= 0x7f8000)
	{
	    //
	    // F was close to FLT_MAX, and the significand was
	    // rounded up, resulting in an exponent overflow.
	    // Avoid the overflow by truncating the significand
	    // instead of rounding it.
	    //

	    i = (e | m) >> 8;
	}
    }

    return (s >> 8) | i;
}


void
notEnoughData ()
{
    throw IEX_NAMESPACE::InputExc ("Error decompressing data "
			 "(input data are shorter than expected).");
}


void
tooMuchData ()
{
    throw IEX_NAMESPACE::InputExc ("Error decompressing data "
			 "(input data are longer than expected).");
}

} // namespace


Pxr24Compressor::Pxr24Compressor (const Header &hdr,
				  size_t maxScanLineSize,
				  size_t numScanLines)
:
    Compressor (hdr),
    _maxScanLineSize (maxScanLineSize),
    _numScanLines (numScanLines),
    _tmpBuffer (0),
    _outBuffer (0),
    _channels (hdr.channels())
{
    size_t maxInBytes =
        uiMult (maxScanLineSize, numScanLines);

    size_t maxOutBytes =
        uiAdd (uiAdd (maxInBytes,
                      size_t (ceil (maxInBytes * 0.01))),
               size_t (100));

    _tmpBuffer = new unsigned char [maxInBytes];
    _outBuffer = new char [maxOutBytes];

    const Box2i &dataWindow = hdr.dataWindow();

    _minX = dataWindow.min.x;
    _maxX = dataWindow.max.x;
    _maxY = dataWindow.max.y;
}


Pxr24Compressor::~Pxr24Compressor ()
{
    delete [] _tmpBuffer;
    delete [] _outBuffer;
}


int
Pxr24Compressor::numScanLines () const
{
    return _numScanLines;
}


Compressor::Format
Pxr24Compressor::format () const
{
    return NATIVE;
}


int
Pxr24Compressor::compress (const char *inPtr,
			   int inSize,
			   int minY,
			   const char *&outPtr)
{
    return compress (inPtr,
	             inSize,
		     Box2i (V2i (_minX, minY),
			    V2i (_maxX, minY + _numScanLines - 1)),
		     outPtr);
}

	      
int
Pxr24Compressor::compressTile (const char *inPtr,
			       int inSize,
			       Box2i range,
			       const char *&outPtr)
{
    return compress (inPtr, inSize, range, outPtr);
}


int
Pxr24Compressor::uncompress (const char *inPtr,
			     int inSize,
			     int minY,
			     const char *&outPtr)
{
    return uncompress (inPtr,
	               inSize,
		       Box2i (V2i (_minX, minY),
			      V2i (_maxX, minY + _numScanLines - 1)),
		       outPtr);
}

		
int
Pxr24Compressor::uncompressTile (const char *inPtr,
				 int inSize,
				 Box2i range,
				 const char *&outPtr)
{
    return uncompress (inPtr, inSize, range, outPtr);
}


int
Pxr24Compressor::compress (const char *inPtr,
			   int inSize,
			   Box2i range,
			   const char *&outPtr)
{
    if (inSize == 0)
    {
	outPtr = _outBuffer;
	return 0;
    }

    int minX = range.min.x;
    int maxX = min (range.max.x, _maxX);
    int minY = range.min.y;
    int maxY = min (range.max.y, _maxY);

    unsigned char *tmpBufferEnd = _tmpBuffer;

    for (int y = minY; y <= maxY; ++y)
    {
	for (ChannelList::ConstIterator i = _channels.begin();
	     i != _channels.end();
	     ++i)
	{
	    const Channel &c = i.channel();

	    if (modp (y, c.ySampling) != 0)
		continue;

	    int n = numSamples (c.xSampling, minX, maxX);

	    unsigned char *ptr[4];
	    unsigned int previousPixel = 0;

	    switch (c.type)
	    {
	      case OPENEXR_IMF_INTERNAL_NAMESPACE::UINT:

		ptr[0] = tmpBufferEnd;
		ptr[1] = ptr[0] + n;
		ptr[2] = ptr[1] + n;
		ptr[3] = ptr[2] + n;
		tmpBufferEnd = ptr[3] + n;

		for (int j = 0; j < n; ++j)
		{
		    unsigned int pixel;
		    char *pPtr = (char *) &pixel;

		    for (size_t k = 0; k < sizeof (pixel); ++k)
			*pPtr++ = *inPtr++;

		    unsigned int diff = pixel - previousPixel;
		    previousPixel = pixel;

		    *(ptr[0]++) = diff >> 24;
		    *(ptr[1]++) = diff >> 16;
		    *(ptr[2]++) = diff >> 8;
		    *(ptr[3]++) = diff;
		}

		break;

	      case OPENEXR_IMF_INTERNAL_NAMESPACE::HALF:

		ptr[0] = tmpBufferEnd;
		ptr[1] = ptr[0] + n;
		tmpBufferEnd = ptr[1] + n;

		for (int j = 0; j < n; ++j)
		{
			Imath_3_1::half pixel;

		    pixel = *(const Imath_3_1::half *) inPtr;
		    inPtr += sizeof (Imath_3_1::half);

		    unsigned int diff = pixel.bits() - previousPixel;
		    previousPixel = pixel.bits();

		    *(ptr[0]++) = diff >> 8;
		    *(ptr[1]++) = diff;
		}

		break;

	      case OPENEXR_IMF_INTERNAL_NAMESPACE::FLOAT:

		ptr[0] = tmpBufferEnd;
		ptr[1] = ptr[0] + n;
		ptr[2] = ptr[1] + n;
		tmpBufferEnd = ptr[2] + n;

		for (int j = 0; j < n; ++j)
		{
		    float pixel;
		    char *pPtr = (char *) &pixel;

		    for (size_t k = 0; k < sizeof (pixel); ++k)
			*pPtr++ = *inPtr++;

		    unsigned int pixel24 = floatToFloat24 (pixel);
		    unsigned int diff = pixel24 - previousPixel;
		    previousPixel = pixel24;

		    *(ptr[0]++) = diff >> 16;
		    *(ptr[1]++) = diff >> 8;
		    *(ptr[2]++) = diff;
		}

		break;

	      default:

		assert (false);
	    }
	}
    }

    uLong inBufferSize = static_cast<uLong> (tmpBufferEnd - _tmpBuffer);
    uLong outSize = compressBound (inBufferSize);

    if (Z_OK != ::compress (
            reinterpret_cast<Bytef*> (_outBuffer),
            &outSize,
            reinterpret_cast<const Bytef*> (_tmpBuffer),
            inBufferSize))
    {
	throw IEX_NAMESPACE::BaseExc ("Data compression (zlib) failed.");
    }

    outPtr = _outBuffer;
    return outSize;
}

 
int		
Pxr24Compressor::uncompress (const char *inPtr,
			     int inSize,
			     Box2i range,
			     const char *&outPtr)
{
    if (inSize == 0)
    {
	outPtr = _outBuffer;
	return 0;
    }

    uLong tmpSize = static_cast<uLong> (_maxScanLineSize * _numScanLines);

    if (Z_OK !=
        ::uncompress (
            reinterpret_cast<Bytef*> (_tmpBuffer),
            &tmpSize,
            reinterpret_cast<const Bytef*> (inPtr),
            inSize))
    {
	throw IEX_NAMESPACE::InputExc ("Data decompression (zlib) failed.");
    }

    int minX = range.min.x;
    int maxX = min (range.max.x, _maxX);
    int minY = range.min.y;
    int maxY = min (range.max.y, _maxY);

    const unsigned char *tmpBufferEnd = _tmpBuffer;
    char *writePtr = _outBuffer;

    for (int y = minY; y <= maxY; ++y)
    {
	for (ChannelList::ConstIterator i = _channels.begin();
	     i != _channels.end();
	     ++i)
	{
	    const Channel &c = i.channel();

	    if (modp (y, c.ySampling) != 0)
		continue;

	    int n = numSamples (c.xSampling, minX, maxX);

	    const unsigned char *ptr[4];
	    unsigned int pixel = 0;

	    switch (c.type)
	    {
	      case OPENEXR_IMF_INTERNAL_NAMESPACE::UINT:

		ptr[0] = tmpBufferEnd;
		ptr[1] = ptr[0] + n;
		ptr[2] = ptr[1] + n;
		ptr[3] = ptr[2] + n;
		tmpBufferEnd = ptr[3] + n;

                if (static_cast<uLong> (tmpBufferEnd - _tmpBuffer) > tmpSize)
                    notEnoughData ();

		for (int j = 0; j < n; ++j)
		{
		    unsigned int diff = (*(ptr[0]++) << 24) |
					(*(ptr[1]++) << 16) |
					(*(ptr[2]++) <<  8) |
					 *(ptr[3]++);

		    pixel += diff;

		    char *pPtr = (char *) &pixel;

		    for (size_t k = 0; k < sizeof (pixel); ++k)
			*writePtr++ = *pPtr++;
		}

		break;

	      case OPENEXR_IMF_INTERNAL_NAMESPACE::HALF:

		ptr[0] = tmpBufferEnd;
		ptr[1] = ptr[0] + n;
		tmpBufferEnd = ptr[1] + n;

                if (static_cast<uLong> (tmpBufferEnd - _tmpBuffer) > tmpSize)
                    notEnoughData ();

		for (int j = 0; j < n; ++j)
		{
		    unsigned int diff = (*(ptr[0]++) << 8) |
					 *(ptr[1]++);

		    pixel += diff;

			Imath_3_1::half * hPtr = (Imath_3_1::half *) writePtr;
		    hPtr->setBits ((unsigned short) pixel);
		    writePtr += sizeof (Imath_3_1::half);
		}

		break;

	      case OPENEXR_IMF_INTERNAL_NAMESPACE::FLOAT:

		ptr[0] = tmpBufferEnd;
		ptr[1] = ptr[0] + n;
		ptr[2] = ptr[1] + n;
		tmpBufferEnd = ptr[2] + n;

                if (static_cast<uLong> (tmpBufferEnd - _tmpBuffer) > tmpSize)
                    notEnoughData ();

		for (int j = 0; j < n; ++j)
		{
		    unsigned int diff = (*(ptr[0]++) << 24) |
					(*(ptr[1]++) << 16) |
					(*(ptr[2]++) <<  8);
		    pixel += diff;

		    char *pPtr = (char *) &pixel;

		    for (size_t k = 0; k < sizeof (pixel); ++k)
			*writePtr++ = *pPtr++;
		}

		break;

	      default:

		assert (false);
	    }
	}
    }

    if (static_cast<uLong> (tmpBufferEnd - _tmpBuffer) < tmpSize) tooMuchData ();

    outPtr = _outBuffer;
    return writePtr - _outBuffer;
}

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_EXIT
