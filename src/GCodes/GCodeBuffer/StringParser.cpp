/*
 * StringParser.cpp
 *
 *  Created on: 6 Feb 2015
 *      Author: David
 */

//*************************************************************************************

#include "StringParser.h"
#include "GCodeBuffer.h"

#include "GCodes/GCodes.h"
#include "Platform.h"
#include "RepRap.h"
#include "General/IP4String.h"

#if HAS_MASS_STORAGE
static constexpr char eofString[] = EOF_STRING;		// What's at the end of an HTML file?
#endif

StringParser::StringParser(GCodeBuffer& gcodeBuffer) noexcept
	: gb(gcodeBuffer), fileBeingWritten(nullptr), writingFileSize(0), eofStringCounter(0), indentToSkipTo(NoIndentSkip),
	  hasCommandNumber(false), commandLetter('Q'), checksumRequired(false), binaryWriting(false)
{
	Init();
}

void StringParser::Init() noexcept
{
	gcodeLineEnd = 0;
	commandLength = 0;
	readPointer = -1;
	hadLineNumber = hadChecksum = false;
	computedChecksum = 0;
	gb.bufferState = GCodeBufferState::parseNotStarted;
	commandIndent = 0;
}

inline void StringParser::AddToChecksum(char c) noexcept
{
	computedChecksum ^= (uint8_t)c;
}

inline void StringParser::StoreAndAddToChecksum(char c)
{
	computedChecksum ^= (uint8_t)c;
	if (gcodeLineEnd < ARRAY_SIZE(gb.buffer))
	{
		gb.buffer[gcodeLineEnd++] = c;
	}
}

// Add a byte to the code being assembled.  If false is returned, the code is
// not yet complete.  If true, it is complete and ready to be acted upon and 'indent'
// is the number of leading white space characters..
bool StringParser::Put(char c)
{
	if (c != 0)
	{
		++commandLength;
	}

	if (c == 0 || c == '\n' || c == '\r')
	{
		return LineFinished();
	}

	if (c == 0x7F && gb.bufferState != GCodeBufferState::discarding)
	{
		// The UART receiver stores 0x7F in the buffer if an overrun or framing errors occurs. So discard the command and resync on the next newline.
		gcodeLineEnd = 0;
		gb.bufferState = GCodeBufferState::discarding;
	}

	// Process the incoming character in a state machine
	bool again;
	do
	{
		again = false;
		switch (gb.bufferState)
		{
		case GCodeBufferState::parseNotStarted:				// we haven't started parsing yet
			switch (c)
			{
			case 'N':
			case 'n':
				hadLineNumber = true;
				AddToChecksum(c);
				gb.bufferState = GCodeBufferState::parsingLineNumber;
				receivedLineNumber = 0;
				break;

			case ' ':
			case '\t':
				AddToChecksum(c);
				++commandIndent;
				break;

			default:
				gb.bufferState = GCodeBufferState::parsingGCode;
				commandStart = 0;
				again = true;
				break;
			}
			break;

		case GCodeBufferState::parsingLineNumber:			// we saw N at the start and we are parsing the line number
			if (isDigit(c))
			{
				AddToChecksum(c);
				receivedLineNumber = (10 * receivedLineNumber) + (c - '0');
				break;
			}
			else
			{
				gb.bufferState = GCodeBufferState::parsingWhitespace;
				again = true;
			}
			break;

		case GCodeBufferState::parsingWhitespace:
			switch (c)
			{
			case ' ':
			case '\t':
				AddToChecksum(c);
				break;

			default:
				gb.bufferState = GCodeBufferState::parsingGCode;
				commandStart = 0;
				again = true;
				break;
			}
			break;

		case GCodeBufferState::parsingGCode:				// parsing GCode words
			switch (c)
			{
			case '*':
				declaredChecksum = 0;
				hadChecksum = true;
				gb.bufferState = GCodeBufferState::parsingChecksum;
				break;

			case ';':
				gb.bufferState = GCodeBufferState::discarding;
				break;

			case '(':
				AddToChecksum(c);
				gb.bufferState = GCodeBufferState::parsingBracketedComment;
				break;

			case '"':
				StoreAndAddToChecksum(c);
				gb.bufferState = GCodeBufferState::parsingQuotedString;
				break;

			default:
				StoreAndAddToChecksum(c);
			}
			break;

		case GCodeBufferState::parsingBracketedComment:		// inside a (...) comment
			AddToChecksum(c);
			if (c == ')')
			{
				gb.bufferState = GCodeBufferState::parsingGCode;
			}
			break;

		case GCodeBufferState::parsingQuotedString:			// inside a double-quoted string
			StoreAndAddToChecksum(c);
			if (c == '"')
			{
				gb.bufferState = GCodeBufferState::parsingGCode;
			}
			break;

		case GCodeBufferState::parsingChecksum:				// parsing the checksum after '*'
			if (isDigit(c))
			{
				declaredChecksum = (10 * declaredChecksum) + (c - '0');
			}
			else
			{
				gb.bufferState = GCodeBufferState::discarding;
				again = true;
			}
			break;

		case GCodeBufferState::discarding:					// discarding characters after the checksum or an end-of-line comment
		default:
			// throw the character away
			break;
		}
	} while (again);

	return false;
}

// This is called when we are fed a null, CR or LF character.
// Return true if there is a completed command ready to be executed.
bool StringParser::LineFinished()
{
	if (gcodeLineEnd == 0)
	{
		// Empty line
		Init();
		return false;
	}

	if (gcodeLineEnd == ARRAY_SIZE(gb.buffer))
	{
		reprap.GetPlatform().MessageF(ErrorMessage, "G-Code buffer '%s' length overflow\n", gb.GetIdentity());
		Init();
		return false;
	}

	gb.buffer[gcodeLineEnd] = 0;
	const bool badChecksum = (hadChecksum && computedChecksum != declaredChecksum);
	const bool missingChecksum = (checksumRequired && !hadChecksum && gb.machineState->previous == nullptr);
	if (reprap.Debug(moduleGcodes) && fileBeingWritten == nullptr)
	{
		reprap.GetPlatform().MessageF(DebugMessage, "%s%s: %s\n", gb.GetIdentity(), ((badChecksum) ? "(bad-csum)" : (missingChecksum) ? "(no-csum)" : ""), gb.buffer);
	}

	if (badChecksum)
	{
		if (hadLineNumber)
		{
			SafeSnprintf(gb.buffer, ARRAY_SIZE(gb.buffer), "M998 P%u", receivedLineNumber);	// request resend
		}
		else
		{
			Init();
			return false;
		}
	}
	else if (missingChecksum)
	{
		// Checksum required but none was provided
		Init();
		return false;
	}

	if (hadLineNumber)
	{
		gb.machineState->lineNumber = receivedLineNumber;
	}
	else
	{
		++gb.machineState->lineNumber;
	}

	if (gb.machineState->DoingFile())
	{
		if (indentToSkipTo < commandIndent)
		{
			Init();
			return false;													// continue skipping this block
		}
		bool skippedIfFalse = false;
		if (indentToSkipTo != NoIndentSkip && indentToSkipTo >= commandIndent)
		{
			// Finished skipping the nested block
			if (indentToSkipTo == commandIndent)
			{
				skippedIfFalse = (gb.machineState->CurrentBlockState().IsIfFalseBlock());
				gb.machineState->CurrentBlockState().SetPlainBlock();		// we've ended the loop or if-block
			}
			indentToSkipTo = NoIndentSkip;									// no longer skipping
		}
		if (ProcessConditionalGCode(skippedIfFalse))
		{
			Init();
			return false;
		}
	}
	commandStart = 0;
	DecodeCommand();
	return true;
}

// Check for and process a conditional GCode language command returning true if we found one, false if it's a regular line of GCode that we need to process
// 'expectingElse' is true if we just finished skipping and if-block when the condition was false and there might be an 'else'
bool StringParser::ProcessConditionalGCode(bool skippedIfFalse)
{
	if (commandIndent > gb.machineState->indentLevel)
	{
		CreateBlocks();					// indentation has increased so start new block(s)
	}
	else if (commandIndent < gb.machineState->indentLevel)
	{
		if (EndBlocks())
		{
			return true;
		}
	}

	// Check for language commands. First count the number of lowercase characters.
	unsigned int i = 0;
	while (gb.buffer[i] >= 'a' && gb.buffer[i] <= 'z')
	{
		++i;
		if (i == 6)
		{
			break;				// all command words are less than 6 characters long
		}
	}

	if (i >= 2 && i < 6 && (gb.buffer[i] == 0 || gb.buffer[i] == ' ' || gb.buffer[i] == '\t'))		// if the command word is properly terminated
	{
		const char * const command = &gb.buffer[commandIndent];
		switch (i)
		{
		case 2:
			if (StringStartsWith(command, "if"))
			{
				ProcessIfCommand();
				return true;
			}
			break;

		case 3:
			if (StringStartsWith(command, "var"))
			{
				ProcessVarCommand();
				return true;
			}
			break;

		case 4:
			if (StringStartsWith(command, "else"))
			{
				ProcessElseCommand(skippedIfFalse);
				return true;
			}
			break;

		case 5:
			if (StringStartsWith(command, "while"))
			{
				ProcessWhileCommand();
				return true;
			}

			if (StringStartsWith(command, "break"))
			{
				ProcessBreakCommand();
				return true;
			}
			break;
		}
	}

	return false;
}

// Create new code blocks
void StringParser::CreateBlocks()
{
	while (gb.machineState->indentLevel < commandIndent)
	{
		gb.machineState->CreateBlock();
	}
}

// End blocks returning true if nothing more to process on this line
bool StringParser::EndBlocks()
{
	while (gb.machineState->indentLevel > commandIndent)
	{
		gb.machineState->EndBlock();
		if (gb.machineState->CurrentBlockState().IsLoop())
		{
			// Go back to the start of the loop and re-evaluate the while-part
			gb.machineState->lineNumber = gb.machineState->CurrentBlockState().GetLineNumber();
			gb.RestartFrom(gb.machineState->CurrentBlockState().GetFilePosition());
			return true;
		}
	}
	return false;
}

void StringParser::ProcessIfCommand()
{
	if (EvaluateCondition("if"))
	{
		gb.machineState->CurrentBlockState().SetIfTrueBlock();
	}
	else
	{
		gb.machineState->CurrentBlockState().SetIfFalseBlock();
		indentToSkipTo = gb.machineState->indentLevel;					// skip forwards to the end of the block
	}
}

void StringParser::ProcessElseCommand(bool skippedIfFalse)
{
	if (skippedIfFalse)
	{
		gb.machineState->CurrentBlockState().SetPlainBlock();			// execute the else-block, treating it like a plain block
	}
	else if (gb.machineState->CurrentBlockState().IsIfTrueBlock())
	{
		indentToSkipTo = gb.machineState->indentLevel;					// skip forwards to the end of the if-block
	}
	else
	{
		throw ConstructParseException("'else' did not follow 'if");
	}
}

void StringParser::ProcessWhileCommand()
{
	if (EvaluateCondition("while"))
	{
		gb.machineState->CurrentBlockState().SetLoopBlock(GetFilePosition(), gb.machineState->lineNumber);
	}
	else
	{
		indentToSkipTo = gb.machineState->indentLevel;					// skip forwards to the end of the block
	}
}

void StringParser::ProcessBreakCommand()
{
	do
	{
		if (gb.machineState->indentLevel == 0)
		{
			throw ConstructParseException("'break' was not inside a loop");
			return;
		}
		gb.machineState->EndBlock();
	} while (!gb.machineState->CurrentBlockState().IsLoop());
	gb.machineState->CurrentBlockState().SetPlainBlock();
}

void StringParser::ProcessVarCommand()
{
	throw ConstructParseException("'var' not implemented yet");
}

// Evaluate the condition that should follow 'if' or 'while'
bool StringParser::EvaluateCondition(const char* keyword)
{
	throw ConstructParseException("Failed to evaluate condition after '%s'", keyword);
}

// Decode this command and find the start of the next one on the same line.
// On entry, 'commandStart' has already been set to the address the start of where the command should be
// and 'commandIndent' is the number of leading whitespace characters at the start of the current line.
// On return, the state must be set to 'ready' to indicate that a command is available and we should stop adding characters.
void StringParser::DecodeCommand()
{
	// Check for a valid command letter at the start
	const char cl = toupper(gb.buffer[commandStart]);
	commandFraction = -1;
	if (cl == 'G' || cl == 'M' || cl == 'T')
	{
		commandLetter = cl;
		hasCommandNumber = false;
		commandNumber = -1;
		parameterStart = commandStart + 1;
		const bool negative = (gb.buffer[parameterStart] == '-');
		if (negative)
		{
			++parameterStart;
		}
		if (isdigit(gb.buffer[parameterStart]))
		{
			hasCommandNumber = true;
			// Read the number after the command letter
			commandNumber = 0;
			do
			{
				commandNumber = (10 * commandNumber) + (gb.buffer[parameterStart] - '0');
				++parameterStart;
			}
			while (isdigit(gb.buffer[parameterStart]));
			if (negative)
			{
				commandNumber = -commandNumber;
			}

			// Read the fractional digit, if any
			if (gb.buffer[parameterStart] == '.')
			{
				++parameterStart;
				if (isdigit(gb.buffer[parameterStart]))
				{
					commandFraction = gb.buffer[parameterStart] - '0';
					++parameterStart;
				}
			}
		}

		// Find where the end of the command is. We assume that a G or M preceded by a space and not inside quotes is the start of a new command.
		bool inQuotes = false;
		bool primed = false;
		for (commandEnd = parameterStart; commandEnd < gcodeLineEnd; ++commandEnd)
		{
			const char c = gb.buffer[commandEnd];
			char c2;
			if (c == '"')
			{
				inQuotes = !inQuotes;
				primed = false;
			}
			else if (!inQuotes)
			{
				if (primed && ((c2 = toupper(c)) == 'G' || c2 == 'M'))
				{
					break;
				}
				primed = (c == ' ' || c == '\t');
			}
		}
	}
	else if (   hasCommandNumber
			 && commandLetter == 'G'
			 && commandNumber <= 3
			 && (   strchr(reprap.GetGCodes().GetAxisLetters(), cl) != nullptr
				 || ((cl == 'I' || cl == 'J') && commandNumber >= 2)
				)
			 && reprap.GetGCodes().GetMachineType() == MachineType::cnc
			)
	{
		// Fanuc-style GCode, repeat the existing G0/G1/G2/G3 command with the new parameters
		parameterStart = commandStart;
		commandEnd = gcodeLineEnd;
	}
	else
	{
		// Bad command
		commandLetter = cl;
		hasCommandNumber = false;
		commandNumber = -1;
		commandFraction = -1;
		parameterStart = commandStart;
		commandEnd = gcodeLineEnd;
	}

	gb.bufferState = GCodeBufferState::ready;
}

// Add an entire string, overwriting any existing content and adding '\n' at the end if necessary to make it a complete line
void StringParser::Put(const char *str, size_t len)
{
	Init();
	for (size_t i = 0; i < len; i++)
	{
		if (Put(str[i]))	// if the line is complete
		{
			return;
		}
	}

	(void)Put('\n');		// because there wasn't one at the end of the string
}

void StringParser::Put(const char *str)
{
	Put(str, strlen(str));
}

void StringParser::SetFinished()
{
	if (commandEnd < gcodeLineEnd)
	{
		// There is another command in the same line of gcode
		commandStart = commandEnd;
		DecodeCommand();
	}
	else
	{
		gb.machineState->g53Active = false;		// G53 does not persist beyond the current line
		Init();
	}
}

// Get the file position at the start of the current command
FilePosition StringParser::GetFilePosition() const noexcept
{
#if HAS_MASS_STORAGE
	if (gb.machineState->DoingFile())
	{
		return gb.machineState->fileState.GetPosition() - gb.fileInput->BytesCached() - commandLength + commandStart;
	}
#endif
	return noFilePosition;
}

const char* StringParser::DataStart() const noexcept
{
	return gb.buffer + commandStart;
}

size_t StringParser::DataLength() const noexcept
{
	return commandEnd - commandStart;
}

// Is 'c' in the G Code string? 'c' must be uppercase.
// Leave the pointer one after it for a subsequent read.
bool StringParser::Seen(char c) noexcept
{
	bool inQuotes = false;
	unsigned int inBrackets = 0;
	for (readPointer = parameterStart; (unsigned int)readPointer < commandEnd; ++readPointer)
	{
		const char b = gb.buffer[readPointer];
		if (b == '"')
		{
			inQuotes = !inQuotes;
		}
		else if (!inQuotes)
		{
			if (inBrackets == 0 && toupper(b) == c && (c != 'E' || (unsigned int)readPointer == parameterStart || !isdigit(gb.buffer[readPointer - 1])))
			{
				++readPointer;
				return true;
			}
			if (b == '{')
			{
				++inBrackets;
			}
			else if (b == '}' && inBrackets != 0)
			{
				--inBrackets;
			}
		}
	}
	readPointer = -1;
	return false;
}

// Get a float after a G Code letter found by a call to Seen()
float StringParser::GetFValue()
{
	if (readPointer > 0)
	{
		const float result = ReadFloatValue();
		readPointer = -1;
		return result;
	}

	THROW_INTERNAL_ERROR;
}

// Get a colon-separated list of floats after a key letter
// If doPad is true then we allow just one element to be given, in which case we fill all elements with that value
void StringParser::GetFloatArray(float arr[], size_t& returnedLength, bool doPad)
{
	if (readPointer > 0)
	{
		size_t length = 0;
		for (;;)
		{
			if (length >= returnedLength)		// array limit has been set in here
			{
				throw ConstructParseException("array too long, max length = %u", (uint32_t)returnedLength);
			}
			arr[length++] = ReadFloatValue();
			if (gb.buffer[readPointer] != LIST_SEPARATOR)
			{
				break;
			}
			++readPointer;
		}

		// Special case if there is one entry and returnedLength requests several. Fill the array with the first entry.
		if (doPad && length == 1 && returnedLength > 1)
		{
			for (size_t i = 1; i < returnedLength; i++)
			{
				arr[i] = arr[0];
			}
		}
		else
		{
			returnedLength = length;
		}

		readPointer = -1;
	}
	else
	{
		THROW_INTERNAL_ERROR;
	}
}

// Get a :-separated list of ints after a key letter
void StringParser::GetIntArray(int32_t arr[], size_t& returnedLength, bool doPad)
{
	if (readPointer > 0)
	{
		size_t length = 0;
		for (;;)
		{
			if (length >= returnedLength) // Array limit has been set in here
			{
				throw ConstructParseException("array too long, max length = %u", (uint32_t)returnedLength);
			}
			arr[length] = ReadIValue();
			length++;
			if (gb.buffer[readPointer] != LIST_SEPARATOR)
			{
				break;
			}
			++readPointer;
		}

		// Special case if there is one entry and returnedLength requests several. Fill the array with the first entry.
		if (doPad && length == 1 && returnedLength > 1)
		{
			for (size_t i = 1; i < returnedLength; i++)
			{
				arr[i] = arr[0];
			}
		}
		else
		{
			returnedLength = length;
		}
		readPointer = -1;
	}
	else
	{
		THROW_INTERNAL_ERROR;
	}
}

// Get a :-separated list of unsigned ints after a key letter
void StringParser::GetUnsignedArray(uint32_t arr[], size_t& returnedLength, bool doPad)
{
	if (readPointer > 0)
	{
		size_t length = 0;
		for (;;)
		{
			if (length >= returnedLength) // Array limit has been set in here
			{
				throw ConstructParseException("array too long, max length = %u", (uint32_t)returnedLength);
			}
			arr[length] = ReadUIValue();
			length++;
			if (gb.buffer[readPointer] != LIST_SEPARATOR)
			{
				break;
			}
			++readPointer;
		}

		// Special case if there is one entry and returnedLength requests several. Fill the array with the first entry.
		if (doPad && length == 1 && returnedLength > 1)
		{
			for (size_t i = 1; i < returnedLength; i++)
			{
				arr[i] = arr[0];
			}
		}
		else
		{
			returnedLength = length;
		}

		readPointer = -1;
	}
	else
	{
		THROW_INTERNAL_ERROR;
	}
}

// Get a :-separated list of drivers after a key letter
void StringParser::GetDriverIdArray(DriverId arr[], size_t& returnedLength)
{
	if (readPointer > 0)
	{
		size_t length = 0;
		for (;;)
		{
			if (length >= returnedLength) // Array limit has been set in here
			{
				throw ConstructParseException("array too long, max length = %u", (uint32_t)returnedLength);
			}
			arr[length] = ReadDriverIdValue();
			length++;
			if (gb.buffer[readPointer] != LIST_SEPARATOR)
			{
				break;
			}
			++readPointer;
		}

		returnedLength = length;
		readPointer = -1;
	}
	else
	{
		THROW_INTERNAL_ERROR;
	}
}

// Get and copy a quoted string returning true if successful
void StringParser::GetQuotedString(const StringRef& str)
{
	str.Clear();
	if (readPointer > 0)
	{
		switch (gb.buffer[readPointer])
		{
		case '"':
			InternalGetQuotedString(str);
			return;

		case '{':
			GetStringExpression(str);
			return;

		default:
			throw ConstructParseException("expected string expression");
		}
	}

	THROW_INTERNAL_ERROR;
}

// Given that the current character is double-quote, fetch the quoted string
void StringParser::InternalGetQuotedString(const StringRef& str)
{
	str.Clear();
	++readPointer;
	for (;;)
	{
		char c = gb.buffer[readPointer++];
		if (c < ' ')
		{
			throw ConstructParseException("control character in string");
		}
		if (c == '"')
		{
			if (gb.buffer[readPointer++] != '"')
			{
				return;
			}
		}
		else if (c == '\'')
		{
			if (isalpha(gb.buffer[readPointer]))
			{
				// Single quote before an alphabetic character forces that character to lower case
				c = tolower(gb.buffer[readPointer++]);
			}
			else if (gb.buffer[readPointer] == c)
			{
				// Two single quotes are used to represent one
				++readPointer;
			}
		}
		str.cat(c);
	}
}

// Get and copy a string which may or may not be quoted. If it is not quoted, it ends at the first space or control character.
void StringParser::GetPossiblyQuotedString(const StringRef& str)
{
	if (readPointer > 0)
	{
		InternalGetPossiblyQuotedString(str, false);
	}

	THROW_INTERNAL_ERROR;
}

// Get and copy a string which may or may not be quoted, starting at readPointer. Return true if successful.
void StringParser::InternalGetPossiblyQuotedString(const StringRef& str, bool allowEmpty)
{
	str.Clear();
	if (gb.buffer[readPointer] == '"')
	{
		InternalGetQuotedString(str);
	}

	if (gb.buffer[readPointer] == '{')
	{
		GetStringExpression(str);
	}

	commandEnd = gcodeLineEnd;				// the string is the remainder of the line of gcode
	for (;;)
	{
		const char c = gb.buffer[readPointer++];
		if (c < ' ')
		{
			break;
		}
		str.cat(c);
	}
	str.StripTrailingSpaces();
	if (!allowEmpty && str.IsEmpty())
	{
		throw ConstructParseException("non-empty string expected");
	}
}

void StringParser::GetReducedString(const StringRef& str)
{
	str.Clear();
	if (readPointer > 0)
	{
		// Reduced strings must start with a double-quote
		if (gb.buffer[readPointer] != '"')
		{
			throw ConstructParseException("string expected");
		}

		++readPointer;
		for (;;)
		{
			const char c = gb.buffer[readPointer++];
			switch(c)
			{
			case '"':
				if (gb.buffer[readPointer++] != '"')
				{
					return;
				}
				str.cat(c);
				break;

			case '_':
			case '-':
			case ' ':
				break;

			default:
				if (c < ' ')
				{
					throw ConstructParseException("control characer in string");
				}
				str.cat(tolower(c));
				break;
			}
		}
	}

	THROW_INTERNAL_ERROR;
}

// This returns a string comprising the rest of the line, excluding any comment
// It is provided for legacy use, in particular in the M23
// command that sets the name of a file to be printed.  In
// preference use GetString() which requires the string to have
// been preceded by a tag letter.
void StringParser::GetUnprecedentedString(const StringRef& str, bool allowEmpty)
{
	readPointer = parameterStart;
	char c;
	while ((unsigned int)readPointer < commandEnd && ((c = gb.buffer[readPointer]) == ' ' || c == '\t'))
	{
		++readPointer;	// skip leading spaces
	}
	InternalGetPossiblyQuotedString(str, allowEmpty);
}

// Get an int32 after a G Code letter
int32_t StringParser::GetIValue()
{
	if (readPointer > 0)
	{
		const int32_t result = ReadIValue();
		readPointer = -1;
		return result;
	}

	THROW_INTERNAL_ERROR;
}

// Get an uint32 after a G Code letter
uint32_t StringParser::GetUIValue()
{
	if (readPointer > 0)
	{
		const uint32_t result = ReadUIValue();
		readPointer = -1;
		return result;
	}

	THROW_INTERNAL_ERROR;
}

// Get a driver ID
DriverId StringParser::GetDriverId()
{
	DriverId result;
	if (readPointer > 0)
	{
		result = ReadDriverIdValue();
		readPointer = -1;
		return result;
	}

	THROW_INTERNAL_ERROR;
}

// Get an IP address quad after a key letter
void StringParser::GetIPAddress(IPAddress& returnedIp)
{
	if (readPointer <= 0)
	{
		THROW_INTERNAL_ERROR;
	}

	const char* p = gb.buffer + readPointer;
	uint8_t ip[4];
	unsigned int n = 0;
	for (;;)
	{
		const char *pp;
		const unsigned long v = SafeStrtoul(p, &pp);
		if (pp == p || v > 255)
		{
			readPointer = -1;
			throw ConstructParseException("invalid IP address");
		}
		ip[n] = (uint8_t)v;
		++n;
		p = pp;
		if (*p != '.')
		{
			break;
		}
		if (n == 4)
		{
			readPointer = -1;
			throw ConstructParseException("invalid IP address");
		}
		++p;
	}
	readPointer = -1;
	if (n != 4)
	{
		throw ConstructParseException("invalid IP address");
	}
	returnedIp.SetV4(ip);
}

// Get a MAX address sextet after a key letter
void StringParser::GetMacAddress(uint8_t mac[6])
{
	if (readPointer <= 0)
	{
		THROW_INTERNAL_ERROR;
	}

	const char* p = gb.buffer + readPointer;
	unsigned int n = 0;
	for (;;)
	{
		const char *pp;
		const unsigned long v = SafeStrtoul(p, &pp, 16);
		if (pp == p || v > 255)
		{
			readPointer = -1;
			throw ConstructParseException("invalid MAC address");
		}
		mac[n] = (uint8_t)v;
		++n;
		p = pp;
		if (*p != ':')
		{
			break;
		}
		if (n == 6)
		{
			readPointer = -1;
			throw ConstructParseException("invalid MAC address");
		}
		++p;
	}
	readPointer = -1;
	if (n != 6)
	{
		throw ConstructParseException("invalid MAC address");
	}
}

// Write the command to a string
void StringParser::PrintCommand(const StringRef& s) const noexcept
{
	s.printf("%c%d", commandLetter, commandNumber);
	if (commandFraction >= 0)
	{
		s.catf(".%d", commandFraction);
	}
}

// Append the full command content to a string
void StringParser::AppendFullCommand(const StringRef &s) const noexcept
{
	s.cat(gb.buffer);
}

#if HAS_MASS_STORAGE

// Open a file to write to
bool StringParser::OpenFileToWrite(const char* directory, const char* fileName, const FilePosition size, const bool binaryWrite, const uint32_t fileCRC32)
{
	fileBeingWritten = reprap.GetPlatform().OpenFile(directory, fileName, OpenMode::writeWithCrc);
	eofStringCounter = 0;
	writingFileSize = size;
	if (fileBeingWritten == nullptr)
	{
		return false;
	}

	crc32 = fileCRC32;
	binaryWriting = binaryWrite;
	return true;
}

// Write the current GCode to file
void StringParser::WriteToFile()
{
	if (GetCommandLetter() == 'M')
	{
		if (GetCommandNumber() == 29)						// end of file?
		{
			fileBeingWritten->Close();
			fileBeingWritten = nullptr;
			SetFinished();
			const char* const r = (gb.MachineState().compatibility == Compatibility::marlin) ? "Done saving file." : "";
			reprap.GetGCodes().HandleReply(gb, GCodeResult::ok, r);
			return;
		}
	}
	else if (GetCommandLetter() == 'G' && GetCommandNumber() == 998)						// resend request?
	{
		if (Seen('P'))
		{
			SetFinished();
			String<ShortScratchStringLength> scratchString;
			scratchString.printf("%" PRIi32 "\n", GetIValue());
			reprap.GetGCodes().HandleReply(gb, GCodeResult::ok, scratchString.c_str());
			return;
		}
	}

	fileBeingWritten->Write(gb.buffer);
	fileBeingWritten->Write('\n');
	SetFinished();
}

void StringParser::WriteBinaryToFile(char b)
{
	if (b == eofString[eofStringCounter] && writingFileSize == 0)
	{
		eofStringCounter++;
		if (eofStringCounter < ARRAY_SIZE(eofString) - 1)
		{
			return;					// not reached end of input yet
		}
	}
	else
	{
		if (eofStringCounter != 0)
		{
			for (uint8_t i = 0; i < eofStringCounter; i++)
			{
				fileBeingWritten->Write(eofString[i]);
			}
			eofStringCounter = 0;
		}
		fileBeingWritten->Write(b);		// writing one character at a time isn't very efficient, but uploading HTML files via USB is rarely done these days
		if (writingFileSize == 0 || fileBeingWritten->Length() < writingFileSize)
		{
			return;					// not reached end of input yet
		}
	}

	FinishWritingBinary();
}

void StringParser::FinishWritingBinary()
{
	// If we get here then we have come to the end of the data
	fileBeingWritten->Close();
	const bool crcOk = (crc32 == fileBeingWritten->GetCRC32() || crc32 == 0);
	fileBeingWritten = nullptr;
	binaryWriting = false;
	if (crcOk)
	{
		const char* const r = (gb.MachineState().compatibility == Compatibility::marlin) ? "Done saving file." : "";
		reprap.GetGCodes().HandleReply(gb, GCodeResult::ok, r);
	}
	else
	{
		reprap.GetGCodes().HandleReply(gb, GCodeResult::error, "CRC32 checksum doesn't match");
	}
}

// This is called when we reach the end of the file we are reading from
void StringParser::FileEnded()
{
	if (IsWritingBinary())
	{
		// We are in the middle of writing a binary file but the input stream has ended
		FinishWritingBinary();
	}
	else
	{
		if (gcodeLineEnd != 0)				// if there is something in the buffer
		{
			Put('\n');						// append a newline in case the file didn't end with one
		}
		if (IsWritingFile())
		{
			bool gotM29 = false;
			if (gb.IsReady())				// if we have a complete command
			{
				gotM29 = (GetCommandLetter() == 'M' && GetCommandNumber() == 29);
				if (!gotM29)				// if it wasn't M29, write it to file
				{
					fileBeingWritten->Write(gb.buffer);
					fileBeingWritten->Write('\n');
				}
			}

			// Close the file whether or not we saw M29
			fileBeingWritten->Close();
			fileBeingWritten = nullptr;
			SetFinished();
			const char* const r = (gb.MachineState().compatibility == Compatibility::marlin) ? "Done saving file." : "";
			reprap.GetGCodes().HandleReply(gb, GCodeResult::ok, r);
		}
	}
}

#endif

// Functions to read values from lines of GCode, allowing for expressions and variable substitution
float StringParser::ReadFloatValue()
{
	++readPointer;
	if (gb.buffer[readPointer] == '{')
	{
		++readPointer;
		const ExpressionValue val = EvaluateExpression();
		switch (val.type)
		{
		case TYPE_OF(float):
			return val.fVal;

		case TYPE_OF(int32_t):
			return (float)val.iVal;

		case TYPE_OF(uint32_t):
			return (float)val.uVal;

		default:
			throw ConstructParseException("expected float value");
		}
	}

	return SafeStrtof(gb.buffer + readPointer, nullptr);
}

uint32_t StringParser::ReadUIValue()
{
	if (gb.buffer[readPointer] == '{')
	{
		const ExpressionValue val = EvaluateExpression();
		switch (val.type)
		{
		case TYPE_OF(uint32_t):
			return val.uVal;

		case TYPE_OF(int32_t):
			if (val.iVal >= 0)
			{
				return (uint32_t)val.iVal;
			}
			throw ConstructParseException("value must be non-negative");

		default:
			throw ConstructParseException("expected non-negative integer value");
		}
	}

	int base = 10;
	size_t skipTrailingQuote = 0;

	// Allow "0xNNNN" or "xNNNN" where NNNN are hex digits
	if (gb.buffer[readPointer] == '"')
	{
		++readPointer;
		skipTrailingQuote = 1;
		switch (gb.buffer[readPointer])
		{
		case 'x':
		case 'X':
			base = 16;
			++readPointer;
			break;

		case '0':
			if (gb.buffer[readPointer + 1] == 'x' || gb.buffer[readPointer + 1] == 'X')
			{
				base = 16;
				readPointer += 2;
			}
			break;

		default:
			break;
		}
	}

	const char *endptr;
	const uint32_t result = SafeStrtoul(gb.buffer + readPointer, &endptr, base);
	readPointer = endptr - gb.buffer + skipTrailingQuote;
	return result;
}

int32_t StringParser::ReadIValue()
{
	if (gb.buffer[readPointer] == '{')
	{
		ExpressionValue val = EvaluateExpression();
		switch (val.type)
		{
		case TYPE_OF(int32_t):
			return val.iVal;

		case TYPE_OF(uint32_t):
			return (int32_t)val.uVal;

		default:
			throw ConstructParseException("expected integer value");
		}
	}

	const char *endptr;
	const int32_t val = SafeStrtol(gb.buffer + readPointer, &endptr);
	readPointer = endptr - gb.buffer;
	return val;
}

DriverId StringParser::ReadDriverIdValue()
{
	DriverId result;
	const uint32_t v1 = ReadUIValue();
#if SUPPORT_CAN_EXPANSION
	if (gb.buffer[readPointer] == '.')
	{
		++readPointer;
		const uint32_t v2 = ReadUIValue();
		result.localDriver = v2;
		result.boardAddress = v1;
	}
	else
	{
		result.localDriver = v1;
		result.boardAddress = 0;
	}
#else
	result.localDriver = v1;
#endif
	return result;
}

// Get a string expression. The current character is '{'.
void StringParser::GetStringExpression(const StringRef& str)
{
	const ExpressionValue val = EvaluateExpression();
	switch (val.type)
	{
	case TYPE_OF(const char*):
		str.copy(val.sVal);
		break;

	case TYPE_OF(float):
		str.printf("%.1f", (double)val.fVal);
		break;

	case TYPE_OF(Float2):
		str.printf("%.2f", (double)val.fVal);
		break;

	case TYPE_OF(Float3):
		str.printf("%.3f", (double)val.fVal);
		break;

	case TYPE_OF(uint32_t):
		str.printf("%" PRIu32, val.uVal);			// convert unsigned integer to string
		break;

	case TYPE_OF(int32_t):
		str.printf("%" PRIi32, val.uVal);			// convert signed integer to string
		break;

	case TYPE_OF(bool):
		str.copy((val.bVal) ? "true" : "false");	// convert bool to string
		break;

	case TYPE_OF(IPAddress):
		str.copy(IP4String(val.uVal).c_str());
		break;

	default:
		throw ConstructParseException("string value expected");
	}
}

// Evaluate an expression. the current character is '{'.
ExpressionValue StringParser::EvaluateExpression()
{
	++readPointer;						// skip the '{'
	// For now the only form of expression we handle is {variable-name}
	if (isalpha(gb.buffer[readPointer]))			// if it's a variable name
	{
		unsigned int start = readPointer;
		unsigned int numBrackets = 0;
		char c;
		while (isalpha((c = gb.buffer[readPointer])) || isdigit(c) || c == '_' || c == '.' || c == '(' || (c == ')' && numBrackets != 0))
		{
			if (c == '(')
			{
				++numBrackets;
			}
			else if (c == ')')
			{
				--numBrackets;
			}
			++readPointer;
		}
		String<MaxVariableNameLength> varName;
		if (varName.copy(gb.buffer + start, readPointer - start))
		{
			throw ConstructParseException("variable name too long");;
		}
		//TODO consider supporting standard CNC functions here
		const ExpressionValue val = reprap.GetObjectValue(*this, varName.c_str());
		if (c != '}')
		{
			throw ConstructParseException("expected '}'");
		}
		++readPointer;
		return val;
	}

	throw ConstructParseException("expected variable name");
}

// Parse a number. the initial character of the string is a decimal digit.
ExpressionValue StringParser::ParseNumber()
{
	// 2. Read digits before decimal point, E or e
	unsigned long valueBeforePoint = 0;
	char c;
	while (isdigit((c = gb.buffer[readPointer])))
	{
		const unsigned int digit = c - '0';
		if (valueBeforePoint > ULONG_MAX/10 || (valueBeforePoint *= 10, valueBeforePoint > ULONG_MAX - digit))
		{
			throw ConstructParseException("too many digits");
		}
		valueBeforePoint += digit;
		++readPointer;
	}

	// 3. Check for decimal point before E or e
	unsigned long valueAfterPoint = 0;
	long digitsAfterPoint = 0;
	bool isFloat = (c == '.');
	if (isFloat)
	{
		++readPointer;

		// 3b. Read the digits (if any) after the decimal point
		while (isdigit((c = gb.buffer[readPointer])))
		{
			const unsigned int digit = c - '0';
			if (valueAfterPoint > ULONG_MAX/10 || (valueAfterPoint *= 10, valueAfterPoint > ULONG_MAX - digit))
			{
				throw ConstructParseException("too many decimal digits");
			}
			valueAfterPoint += digit;
			++digitsAfterPoint;
			++readPointer;
		}
	}

	// 5. Check for exponent part
	long exponent = 0;
	if (toupper(c) == 'E')
	{
		isFloat = true;
		++readPointer;
		c = gb.buffer[readPointer];

		// 5a. Check for signed exponent
		const bool expNegative = (c == '-');
		if (expNegative || c == '+')
		{
			++readPointer;
		}

		// 5b. Read exponent digits
		while (isdigit((c = gb.buffer[readPointer])))
		{
			exponent = (10 * exponent) + (c - '0');	// could overflow, but anyone using such large numbers is being very silly
			++readPointer;
		}

		if (expNegative)
		{
			exponent = -exponent;
		}
	}

	// 6. Compute the composite value
	ExpressionValue retvalue;

	if (isFloat)
	{
		retvalue.type = TypeOf<float>();
		if (valueAfterPoint != 0)
		{
			if (valueBeforePoint == 0)
			{
				retvalue.fVal = (float)((double)valueAfterPoint * pow(10, exponent - digitsAfterPoint));
			}
			else
			{
				retvalue.fVal = (float)(((double)valueAfterPoint/pow(10, digitsAfterPoint) + valueBeforePoint) * pow(10, exponent));
			}
		}
		else
		{
			retvalue.fVal = (float)(valueBeforePoint * pow(10, exponent));
		}
	}
	else
	{
		retvalue.type = TypeOf<uint32_t>();
		retvalue.uVal = valueBeforePoint;
	}

	return retvalue;
}

ParseException StringParser::ConstructParseException(const char *str) const
{
	return ParseException(readPointer, str);
}

ParseException StringParser::ConstructParseException(const char *str, const char *param) const
{
	return ParseException(readPointer, str, param);
}

ParseException StringParser::ConstructParseException(const char *str, uint32_t param) const
{
	return ParseException(readPointer, str, param);
}

// End
