/* Copyright is waived. No warranty is provided. Unrestricted use and modification is permitted. */

#include <cstring>
#include <cstdio>
#include <iostream>
#include <fstream>

using std::cout;
using std::endl;
using std::string;


void help() {
	cout << "LZSS Compressor/Decompressor" << endl << endl;
    cout << "lzss [-c|-d] input_file output_file" << endl << endl;
	cout << "  -c   compress input_file to output_file" << endl;
	cout << "  -d   decompress input_file to output_file" << endl << endl;
}


void error(const std::string &error) {
    cout << "Error: " << error << endl;
    exit(EXIT_FAILURE);
}


int compress(const void* input, int inputLength, void* output, int outputLength, int dictionaryLength)
{
	// Ensure the dictionary length is legal
	if ((dictionaryLength & (dictionaryLength - 1)) != 0)
	{
		error ("Dictionary length must be a power of 2");
	}
	if (dictionaryLength < 4)
	{
		error ("Dictionary length can not be less than 4 bytes");
	}
	if (dictionaryLength > 16384)
	{
		error ("Dictionary length can exceed 16384 bytes");
	}

	// Ensure the destination buffer is big enough for at least the header information
	if (outputLength < ((int)(sizeof(int) * 2)))
	{
		error ("Destination buffer is too small");
	}

	// Write header data to the destination buffer
	auto header = (int*) output;
	*header++ = inputLength;		// Write the uncompressed data length
	*header++ = dictionaryLength;	// Write the dictionary length

	// Calculate the maximum offset and maximum match length for this dictionary length
	int maxOffset = dictionaryLength + 2;
	int maxMatch = (65536 / dictionaryLength) + 2;
	int lengthShift = -1;
	while (dictionaryLength) { lengthShift++; dictionaryLength >>= 1; }

	// The compressed data consists of 3 separate streams of data; bit flags, strings, and bytes.
	// The bit flag indicates whether the next element of data is a string or a byte. The string is
	// a 16-bit value containing an offset and a length from which a string should be copied. The byte
	// value is simply a literal that should be stored. The data for each stream is written 32-bits at
	// a time, therefore we accumulate 32 bit flags, 2 strings (16-bits each), or 4 bytes (8-bits each)
	// before storing them. The data for each stream is written as soon as it is accumulated, hence the
	// streams are interleaved in memory. On decompression, the compressed data can be read linearly
	// with the data for each stream arriving exactly as its needed.

	int* next = header;
	int* outputEnd = (int*)((unsigned char*)output + outputLength);
	int* nextBits = nullptr;
	int* nextBytes = nullptr;
	int* nextStrings = nullptr;
	int  bits = 0;			// Bit accumulator
	int  bitMask = 0;
	int  bytes = 0;			// Byte accumulator
	int  byteCount = 0;
	int  strings = 0;		// String accumulator
	int  stringCount = 0;

	// Compress data
	auto start = (unsigned char*)input;
	auto current = (unsigned char*)input;
	auto end = (unsigned char*)input + inputLength;
	while (current < end)
	{
		// Find the start of the search window
		unsigned char* search = current - maxOffset;
		if (search < start) search = start;

		// Find the longest match in the search window
		int bestLength = 0;
		int bestOffset = 0;
		while ((search + bestLength) <= current)
		{
			unsigned char* p1 = search;
			unsigned char* p2 = current;
			int matchLength = 0;

			while ((*p1 == *p2) && (p1 < current) && (matchLength < maxMatch) && (p2 < end))
			{
				p1++;
				p2++;
				matchLength++;
			}

			if (matchLength >= bestLength)
			{
				bestLength = matchLength;
				bestOffset = (int)(current - search);
			}

			search++;
		}

		// If the bit accumulator is empty then reserve memory for the next 32-bits 
		if (bitMask == 0)
		{
			if (next == outputEnd) return false;		// fail if output buffer is too small
			nextBits = next++;
			bits = 0;
			bitMask = 1;
		}

		// Did we find a matching string of more than 2 bytes?
		if (bestLength > 2)
		{
			// Write a 1 bit to the bit stream to indicate next item is a string
			// If we have accumulated 32-bits then flush the bit flags to memory
			bits |= bitMask;
			bitMask <<= 1;
			if (!bitMask) *nextBits = bits;

			// If the string accumulator is empty then reserve memory for the next 2 strings
			if (stringCount == 0)
			{
				if (next == outputEnd) return false;		// fail if output buffer is too small
				nextStrings = next++;
				strings = 0;
			}

			// Add the string offset and size to the offset accumulator
			strings += (((bestLength - 3) << lengthShift) + (bestOffset - 3)) << (stringCount * 16);
			stringCount++;

			// If we have accumulated 2 strings then flush them to memory
			if (stringCount == 2)
			{
				*nextStrings = strings;
				stringCount = 0;
			}

			// Move along the current pointer by the size of the match
			current += bestLength;
		}
		else
		{
			// Write a 0 bit to the bitstream to indicate next item is a byte literal
			// If we have accumulated 32-bits then flush the bit flags to memory
			bitMask <<= 1;					// By shifting the bitMask we are implicitly writing a 0 to the bit flags
			if (!bitMask) *nextBits = bits;

			// If the byte accumulator is empty then reserve memory for the next 4 bytes
			if (byteCount == 0)
			{
				if (next == outputEnd) return false;		// fail if output buffer is too small
				nextBytes = next++;
				bytes = 0;
			}

			// Add the current byte value to the byte accumulator
			bytes += (*current++) << (byteCount * 8);
			byteCount++;

			// If we have accumulated 4 bytes then flush them to memory
			if (byteCount == 4)
			{
				*nextBytes = bytes;
				byteCount = 0;
			}
		}
	}

	// Write any remaining data out to their respective streams
	if (bitMask)     *nextBits = bits;
	if (byteCount)   *nextBytes = bytes;
	if (stringCount) *nextStrings = strings;

	// Calculate and return the size of the compressed data
	int compressedLength = (int)((char*)next - (char*)output);
	return compressedLength;
}


int decompress(const void* input, void* output, int outputBufferLength)
{
	// Read the header information
	auto current = (int*)input;
	int  uncompressedLength = *current++;	// Read the uncompressed data length
	int  dictionaryLength = *current++;		// Read the dictionary length

	// Make sure the output buffer is big enough
	if (outputBufferLength < uncompressedLength)
	{
		error ("Destination buffer is too small");
	}

	// Calculate values needed to separate strings into their offset and length components
	int offsetMask = dictionaryLength - 1;
	int lengthShift = -1;
	while (dictionaryLength) { lengthShift++; dictionaryLength >>= 1; }
	int lengthMask = ((~offsetMask) & 0xffff) >> lengthShift;

	// Initialize accumulators
	int bits = 0;
	int bytes = 0;
	int strings = 0;
	int bitMask = 0;
	int byteCount = 0;
	int stringCount = 0;

	// Decompress data
	auto buffer = (unsigned char*)output;
	int remaining = uncompressedLength;
	while (remaining)
	{
		// If the bit accumulator is empty then fill it
		if (!bitMask)
		{
			bits = *current++;
			bitMask = 1;
		}

		// Is next item a byte or a string?
		if (bits & bitMask)
		{
			// If the string accumulator is empty then fill it
			if (!stringCount)
			{
				strings = *current++;
				stringCount = 2;
			}

			// It's a string, so copy it
			unsigned char* p = buffer - (strings & offsetMask) - 3;
			int length = ((strings >> lengthShift) & lengthMask) + 3;
			memcpy(buffer, p, (size_t)length);
			buffer += length;
			remaining -= length;

			strings >>= 16;
			stringCount--;
		}
		else
		{
			// If the byte accumulator is empty then fill it
			if (!byteCount)
			{
				bytes = *current++;
				byteCount = 4;
			}

			// Write the next byte
			*buffer++ = (unsigned char)(bytes & 0xff);
			remaining--;

			bytes >>= 8;
			byteCount--;
		}

		// Shift bit mask for next data element
		bitMask <<= 1;
	}

	return uncompressedLength;
}


int getDecompressedLength(const void* input)
{
	return *(int*)input;
}


int main(int argc, const char *argv[]) {

    // Parse command line
    if (argc < 4) {
        help();
        exit(EXIT_SUCCESS);
    }

	const string mode(argv[1]);
    const string input_file(argv[2]);
    const string output_file(argv[3]);

	if (mode == "-c")
	{
		// Read input file
		cout << "Compressing " + input_file;
		std::ifstream ifs(input_file, std::ifstream::binary);
		if (ifs)
		{
			// Read input file
			ifs.seekg(0, std::ifstream::end);
			int input_length = (int) ifs.tellg();
			ifs.seekg(0, std::ifstream::beg);
			char* input_buffer = new char[input_length];
			ifs.read(input_buffer, input_length);
			ifs.close();

			// Compress file
			int output_buffer_length = (input_length * 2) + 1024;		// expect that the compressed length will never be more than this
			char* output_buffer = new char[output_buffer_length];
			int output_length = compress(input_buffer, input_length, output_buffer, output_buffer_length, 8192);

			// Write compressed file
			if (output_length > 0)
			{
				std::ofstream ofs(output_file, std::ifstream::binary + std::ifstream::out);
				ofs.write(output_buffer, output_length);
				ofs.close();
			}
		}
		else
		{
			error("Unable to open input file " + input_file);
		}
	}
	else if (mode == "-d")
	{
		// Read input file
		cout << "Decompressing " + input_file;
		std::ifstream ifs(input_file, std::ifstream::binary);
		if (ifs)
		{
			ifs.seekg(0, std::ifstream::end);
			int input_length = (int) ifs.tellg();
			ifs.seekg(0, std::ifstream::beg);
			char* input_buffer = new char[input_length];
			ifs.read(input_buffer, input_length);
			ifs.close();

			// Decompress file
			int output_buffer_length = getDecompressedLength(input_buffer);
			char* output_buffer = new char[output_buffer_length];
			int output_length = decompress(input_buffer, output_buffer, output_buffer_length);

			// Write decompressed file
			std::ofstream ofs(output_file, std::ifstream::binary + std::ifstream::out);
			ofs.write(output_buffer, output_length);
			ofs.close();
		}
		else
		{
			error("Unable to open input file " + input_file);
		}
	}
	else
	{
		error("Unknown option " + mode);
	}

    return EXIT_SUCCESS;
}
