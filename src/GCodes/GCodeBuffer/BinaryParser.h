/*
 * BinaryGCodeBuffer.h
 *
 *  Created on: 30 Mar 2019
 *      Author: Christian
 */

#ifndef SRC_GCODES_GCODEBUFFER_BINARYGCODEBUFFER_H_
#define SRC_GCODES_GCODEBUFFER_BINARYGCODEBUFFER_H_

#include "Linux/MessageFormats.h"
#include "MessageType.h"
#include "RepRapFirmware.h"
#include "ParseException.h"

class GCodeBuffer;
class IPAddress;

class BinaryParser
{
public:
	BinaryParser(GCodeBuffer& gcodeBuffer);
	void Init(); 								// Set it up to parse another G-code
	void Put(const char *data, size_t len);		// Add an entire string, overwriting any existing content
	bool Seen(char c) __attribute__((hot));		// Is a character present?

	char GetCommandLetter() const;
	bool HasCommandNumber() const;
	int GetCommandNumber() const;
	int8_t GetCommandFraction() const;

	float GetFValue() THROWS_PARSE_ERROR __attribute__((hot));				// Get a float after a key letter
	int32_t GetIValue() THROWS_PARSE_ERROR __attribute__((hot));			// Get an integer after a key letter
	uint32_t GetUIValue() THROWS_PARSE_ERROR;								// Get an unsigned integer value
	DriverId GetDriverId() THROWS_PARSE_ERROR;								// Get a driver ID
	void GetIPAddress(IPAddress& returnedIp) THROWS_PARSE_ERROR;			// Get an IP address quad after a key letter
	void GetMacAddress(uint8_t mac[6]) THROWS_PARSE_ERROR;					// Get a MAC address sextet after a key letter
	void GetUnprecedentedString(const StringRef& str, bool allowEmpty) THROWS_PARSE_ERROR;	// Get a string with no preceding key letter
	void GetQuotedString(const StringRef& str) THROWS_PARSE_ERROR;			// Get and copy a quoted string
	void GetPossiblyQuotedString(const StringRef& str) THROWS_PARSE_ERROR;	// Get and copy a string which may or may not be quoted
	void GetReducedString(const StringRef& str) THROWS_PARSE_ERROR;			// Get and copy a quoted string, removing certain characters
	void GetFloatArray(float arr[], size_t& length, bool doPad) THROWS_PARSE_ERROR __attribute__((hot)); // Get a colon-separated list of floats after a key letter
	void GetIntArray(int32_t arr[], size_t& length, bool doPad) THROWS_PARSE_ERROR;		// Get a :-separated list of ints after a key letter
	void GetUnsignedArray(uint32_t arr[], size_t& length, bool doPad) THROWS_PARSE_ERROR;	// Get a :-separated list of unsigned ints after a key letter
	void GetDriverIdArray(DriverId arr[], size_t& length) THROWS_PARSE_ERROR;	// Get a :-separated list of drivers after a key letter

	void SetFinished();									// Set the G Code finished

	FilePosition GetFilePosition() const;				// Get the file position at the start of the current command

	const char* DataStart() const;						// Get the start of the current command
	size_t DataLength() const;							// Get the length of the current command

	void PrintCommand(const StringRef& s) const;
	void AppendFullCommand(const StringRef &s) const;

private:
	GCodeBuffer& gb;

	ParseException ConstructParseException(const char *str) const;
	ParseException ConstructParseException(const char *str, const char *param) const;
	ParseException ConstructParseException(const char *str, uint32_t param) const;

	size_t AddPadding(size_t bytesRead) const;
	template<typename T> void GetArray(T arr[], size_t& length, bool doPad) __attribute__((hot));
	void WriteParameters(const StringRef& s, bool quoteStrings) const;

	size_t bufferLength;
	const CodeHeader *header;

	int reducedBytesRead;
	const CodeParameter *seenParameter;
	const char *seenParameterValue;
};

#endif /* SRC_GCODES_GCODEBUFFER_BINARYGCODEBUFFER_H_ */
